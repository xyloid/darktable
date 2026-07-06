"""Shared test infrastructure: a fake, in-process darktable remote-edit
peer, and fixture-loading helpers.

The fake server speaks the exact wire framing (`src/control/remote_frame.h`)
and hello/dispatch shapes described in
docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md, using
plain `asyncio.start_server` on loopback -- no darktable, no C code, no
sockets beyond what the test process itself opens. It deliberately does not
reimplement every C-side transport policy (the 3-auth-failure connection
close, the 4-connection/8-pending-request caps): those are `remote_server.c`
behaviors with their own C unit tests. This fake only needs to be a faithful
wire peer for what `protocol.py` itself is responsible for: framing,
hello, request/response correlation, timeouts, and reconnection.
"""

from __future__ import annotations

import asyncio
import json
import struct
from pathlib import Path
from typing import Any, Callable

import pytest

# tools/mcp/tests/conftest.py -> parents[0]=tests, [1]=mcp, [2]=tools,
# [3]=repository root. Documented assumption (see tools/mcp/README.md):
# these tests only pass when run from inside a checkout of this
# repository, not from an installed copy of the darktable_mcp package.
FIXTURES_DIR = (
    Path(__file__).resolve().parents[3] / "src" / "tests" / "unittests" / "control" / "fixtures"
)


def load_fixture(name: str) -> dict:
    return json.loads((FIXTURES_DIR / name).read_text(encoding="utf-8"))


class WireError(Exception):
    """Raised by a `FakeDarktableServer` method handler to produce an
    `{"ok": false, "error": {...}}` response."""

    def __init__(self, code: str, message: str, details: Any = None, retryable: bool = False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details
        self.retryable = retryable


def result_handler_from_fixture(response_fixture: str) -> Callable[[dict | None], dict]:
    """Builds a method handler from one of the shared C-dispatcher
    fixtures: a success fixture's `result` is returned verbatim; an error
    fixture's `error` is raised as a `WireError` so the fake server's
    normal error-response path produces the same shape the C dispatcher
    would have.
    """
    data = load_fixture(response_fixture)
    if data.get("ok"):
        result = data["result"]
        return lambda _params: result

    error = data["error"]

    def _raise(_params: dict | None) -> dict:
        raise WireError(
            error["code"], error["message"], error.get("details"), error.get("retryable", False)
        )

    return _raise


HelloHandler = Callable[[dict | None, Any], dict]
MethodHandler = Callable[[dict | None], dict]


class FakeDarktableServer:
    """A minimal stand-in for darktable's remote-edit server."""

    def __init__(self, *, token: str = "test-token", protocol_version: int = 1):
        self.token = token
        self.protocol_version = protocol_version
        self.handlers: dict[str, MethodHandler] = {}
        self.hello_override: HelloHandler | None = None
        self.delay_before_response: dict[str, float] = {}
        self.close_after_hello = False
        self.drop_requests: set[str] = set()  # methods to silently never answer
        self._server: asyncio.base_events.Server | None = None
        self.connections_seen = 0

    def handle(self, method: str, fn: MethodHandler) -> None:
        self.handlers[method] = fn

    def handle_from_fixture(self, method: str, response_fixture: str) -> None:
        self.handle(method, result_handler_from_fixture(response_fixture))

    async def start(self) -> FakeDarktableServer:
        self._server = await asyncio.start_server(self._on_client, "127.0.0.1", 0)
        return self

    @property
    def port(self) -> int:
        assert self._server is not None
        return self._server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self._server is None:
            return
        self._server.close()
        await self._server.wait_closed()

    async def __aenter__(self) -> FakeDarktableServer:
        return await self.start()

    async def __aexit__(self, *exc: object) -> None:
        await self.stop()

    @staticmethod
    async def _read_frame(reader: asyncio.StreamReader) -> dict:
        header = await reader.readexactly(4)
        (length,) = struct.unpack(">I", header)
        body = await reader.readexactly(length)
        return json.loads(body.decode("utf-8"))

    @staticmethod
    def _write_frame(writer: asyncio.StreamWriter, message: dict) -> None:
        data = json.dumps(message).encode("utf-8")
        writer.write(struct.pack(">I", len(data)) + data)

    def _default_hello(self, params: dict | None, req_id: Any) -> dict:
        token = (params or {}).get("token")
        if token != self.token:
            return {
                "id": req_id,
                "ok": False,
                "error": {"code": "unauthorized", "message": "invalid token", "retryable": False},
            }
        return {
            "id": req_id,
            "ok": True,
            "result": {
                "protocol_version": self.protocol_version,
                "darktable_version": "5.x",
                "pid": 12345,
                "capabilities": [],
            },
        }

    async def _on_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.connections_seen += 1
        authed = False
        try:
            while True:
                try:
                    req = await self._read_frame(reader)
                except asyncio.IncompleteReadError:
                    return

                req_id = req.get("id")
                method = req.get("method")
                params = req.get("params")

                if not authed:
                    if method != "hello":
                        response = {
                            "id": req_id,
                            "ok": False,
                            "error": {
                                "code": "unauthorized",
                                "message": "hello must be the first frame",
                                "retryable": False,
                            },
                        }
                    elif self.hello_override is not None:
                        response = self.hello_override(params, req_id)
                    else:
                        response = self._default_hello(params, req_id)

                    authed = response.get("ok") is True
                    self._write_frame(writer, response)
                    await writer.drain()
                    if self.close_after_hello and authed:
                        writer.close()
                        return
                    continue

                if method in self.drop_requests:
                    continue  # simulate a request darktable never answers

                delay = self.delay_before_response.get(method)
                if delay:
                    await asyncio.sleep(delay)

                handler = self.handlers.get(method)
                if handler is None:
                    response = {
                        "id": req_id,
                        "ok": False,
                        "error": {
                            "code": "invalid_value",
                            "message": f"unknown method '{method}'",
                            "retryable": False,
                        },
                    }
                else:
                    try:
                        response = {"id": req_id, "ok": True, "result": handler(params)}
                    except WireError as exc:
                        error: dict[str, Any] = {
                            "code": exc.code,
                            "message": exc.message,
                            "retryable": exc.retryable,
                        }
                        if exc.details is not None:
                            error["details"] = exc.details
                        response = {"id": req_id, "ok": False, "error": error}

                self._write_frame(writer, response)
                await writer.drain()
        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            writer.close()


@pytest.fixture
def fixtures_dir() -> Path:
    return FIXTURES_DIR


@pytest.fixture
def fake_server_factory():
    """Yields a factory for `FakeDarktableServer`; tracks every instance
    created so the fixture can best-effort close any leaked listener
    socket at teardown even if a test forgets its own `stop()`.

    Deliberately does NOT call `wait_closed()`/anything else that needs a
    running event loop here: by teardown time (a plain sync generator
    fixture), the test's own event loop (owned by pytest-asyncio, one per
    test in "auto" mode) may already be gone, and `asyncio.Server`
    objects are bound to the loop that created them -- awaiting them from
    a *different* loop raises. `Server.close()` itself is a plain,
    non-async call that only marks the listening socket for closing, so
    it is safe to call here regardless of loop lifetime; the OS reclaims
    the socket even without an explicit `wait_closed()`.
    """
    created: list[FakeDarktableServer] = []

    async def _make(**kwargs: Any) -> FakeDarktableServer:
        server = FakeDarktableServer(**kwargs)
        await server.start()
        created.append(server)
        return server

    yield _make

    for server in created:
        if server._server is not None:
            server._server.close()
