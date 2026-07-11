"""Framed-JSON client for the darktable remote-edit wire protocol.

Wire facts (fixed by the C implementation, `src/control/remote_frame.h` and
`src/control/remote_server.c`; see docs/superpowers/specs/2026-07-05-
darktable-mcp-protocol-reference.md for the full method contract):

* Framing: a 4-byte big-endian payload length, then that many bytes of
  UTF-8 JSON. Zero-length and >16 MiB frames are protocol errors in both
  directions.
* The first frame on a connection MUST be `hello` with the session token;
  anything else gets `unauthorized` and, after 3 auth failures, the
  connection is closed by the peer.
* Every other request is `{"id": <uint>, "method": ..., "params": {...}?}`;
  every response is `{"id", "ok": true, "result": ...}` or
  `{"id", "ok": false, "error": {"code", "message", "details"?, "retryable"}}`.

This module has no dependency on the MCP SDK (see the package README):
only `server.py` imports `mcp`. It is plain `asyncio`, usable from a script,
a test, or a different agent framework.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import struct
from typing import Any

from .discovery import DiscoveryRecord
from .errors import ProtocolError, RequestOutcomeUnknown, TransportError

HEADER_LEN = 4
MAX_FRAME = 16 * 1024 * 1024
DEFAULT_CLIENT_NAME = "darktable-mcp/0.1.0"
PROTOCOL_VERSION = 1

DEFAULT_CONNECT_TIMEOUT = 5.0
DEFAULT_REQUEST_TIMEOUT = 10.0


def encode_frame(payload: dict[str, Any]) -> bytes:
    """Serializes `payload` to UTF-8 JSON and prefixes it with its 4-byte
    big-endian length. Raises `TransportError` if the encoded frame would
    exceed `MAX_FRAME` (rather than sending something the server is
    guaranteed to reject as a corrupt/oversized header)."""
    data = json.dumps(payload).encode("utf-8")
    if not data:
        raise TransportError("refusing to send a zero-length frame")
    if len(data) > MAX_FRAME:
        raise TransportError(
            f"request too large to send ({len(data)} bytes > {MAX_FRAME} byte frame cap)"
        )
    return struct.pack(">I", len(data)) + data


class _Connection:
    """One authenticated TCP connection: owns the socket, the read loop,
    and request/response correlation by id. Not reconnect-aware -- that is
    `ProtocolClient`'s job, one layer up. Not thread-safe; use from a
    single asyncio task/event loop.
    """

    def __init__(
        self,
        host: str,
        port: int,
        token: str,
        *,
        client_name: str = DEFAULT_CLIENT_NAME,
        protocol_version: int = PROTOCOL_VERSION,
        connect_timeout: float = DEFAULT_CONNECT_TIMEOUT,
        request_timeout: float = DEFAULT_REQUEST_TIMEOUT,
    ) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.client_name = client_name
        self.protocol_version = protocol_version
        self.connect_timeout = connect_timeout
        self.request_timeout = request_timeout

        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._read_task: asyncio.Task[None] | None = None
        self._next_id = itertools.count(1)
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._closed = False
        self.server_info: dict[str, Any] | None = None

    def is_connected(self) -> bool:
        return self._writer is not None and not self._closed

    async def connect(self) -> dict[str, Any]:
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port), timeout=self.connect_timeout
            )
        except asyncio.TimeoutError as exc:
            raise TransportError(
                f"timed out connecting to darktable at {self.host}:{self.port}"
            ) from exc
        except OSError as exc:
            raise TransportError(
                f"could not connect to darktable at {self.host}:{self.port}: {exc}"
            ) from exc

        self._read_task = asyncio.ensure_future(self._read_loop())

        try:
            self.server_info = await self._request(
                "hello",
                {
                    "protocol_version": self.protocol_version,
                    "token": self.token,
                    "client": self.client_name,
                },
                timeout=self.connect_timeout,
            )
        except Exception:
            await self.close()
            raise

        return self.server_info

    async def _read_loop(self) -> None:
        assert self._reader is not None
        try:
            while True:
                try:
                    header = await self._reader.readexactly(HEADER_LEN)
                except asyncio.IncompleteReadError as exc:
                    if exc.partial:
                        raise TransportError(
                            "darktable closed the connection mid-frame"
                        ) from exc
                    raise TransportError("darktable closed the connection") from exc

                (length,) = struct.unpack(">I", header)
                if length == 0 or length > MAX_FRAME:
                    raise TransportError(f"darktable sent an invalid frame length {length}")

                try:
                    body = await self._reader.readexactly(length)
                except asyncio.IncompleteReadError as exc:
                    raise TransportError(
                        "darktable closed the connection mid-frame"
                    ) from exc

                try:
                    message = json.loads(body.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise TransportError(f"darktable sent malformed JSON: {exc}") from exc

                self._dispatch(message)
        except TransportError as exc:
            self._fail_pending(exc)
        except Exception as exc:  # pragma: no cover - defensive catch-all
            self._fail_pending(TransportError(f"read loop failed: {exc}"))

    def _dispatch(self, message: Any) -> None:
        if not isinstance(message, dict):
            return
        request_id = message.get("id")
        future = self._pending.pop(request_id, None)
        if future is not None and not future.done():
            future.set_result(message)

    def _fail_pending(self, exc: Exception) -> None:
        self._closed = True
        for future in self._pending.values():
            if not future.done():
                future.set_exception(exc)
        self._pending.clear()

    async def _request(
        self, method: str, params: dict[str, Any] | None = None, *, timeout: float | None = None
    ) -> dict[str, Any]:
        assert self._writer is not None
        request_id = next(self._next_id)
        payload: dict[str, Any] = {"id": request_id, "method": method}
        if params is not None:
            payload["params"] = params

        frame = encode_frame(payload)

        loop = asyncio.get_event_loop()
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[request_id] = future

        try:
            self._writer.write(frame)
            await self._writer.drain()
        except (ConnectionError, OSError) as exc:
            self._pending.pop(request_id, None)
            # Nothing can have reached darktable if write() itself failed
            # synchronously on a dead socket -- but drain() failing after a
            # partial write is genuinely ambiguous, so treat both the same
            # conservative way: unknown outcome, do not retry automatically.
            raise RequestOutcomeUnknown(
                f"failed to send {method!r} (request id {request_id}): {exc}"
            ) from exc

        try:
            message = await asyncio.wait_for(future, timeout=timeout or self.request_timeout)
        except asyncio.TimeoutError as exc:
            self._pending.pop(request_id, None)
            raise RequestOutcomeUnknown(
                f"timed out waiting for a response to {method!r} "
                f"(request id {request_id}); darktable may or may not have processed it"
            ) from exc
        except TransportError as exc:
            raise RequestOutcomeUnknown(
                f"connection lost while waiting for a response to {method!r} "
                f"(request id {request_id}); darktable may or may not have processed it"
            ) from exc

        return _decode_response(message)

    async def request(
        self, method: str, params: dict[str, Any] | None = None, *, timeout: float | None = None
    ) -> dict[str, Any]:
        if not self.is_connected():
            raise TransportError("not connected")
        return await self._request(method, params, timeout=timeout)

    async def close(self) -> None:
        self._closed = True
        if self._read_task is not None:
            self._read_task.cancel()
            self._read_task = None
        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except OSError:
                pass
            self._writer = None
        self._reader = None
        self._fail_pending(TransportError("connection closed"))


def _decode_response(message: dict[str, Any]) -> dict[str, Any]:
    if message.get("ok"):
        result = message.get("result")
        return result if isinstance(result, dict) else {}

    error = message.get("error") or {}
    raise ProtocolError(
        code=error.get("code", "internal"),
        message=error.get("message", "unknown error"),
        details=error.get("details"),
        retryable=bool(error.get("retryable", False)),
    )


class ProtocolClient:
    """Reconnect-aware wrapper around `_Connection`, addressed to one
    discovery record. This is the class `server.py` (and any other
    caller) should use.

    Reconnect policy: a fresh connection is opened lazily on the first
    `call()` and reused for subsequent calls. If a call fails with an
    unambiguous transport problem (refused connection, clean EOF between
    requests, malformed framing), the dead connection is dropped so the
    *next* `call()` reconnects automatically -- but the request that
    failed is never retried automatically, and its exception (typically
    `RequestOutcomeUnknown` or `TransportError`) always propagates to the
    caller. This is deliberate: for a request whose outcome darktable-side
    is unknown, silently retrying could double-apply a future mutation;
    surfacing the uncertainty and letting the caller decide is the only
    safe default. For the read-only tools in this package, replaying a
    failed read-only call by hand is always safe.
    """

    def __init__(
        self,
        record: DiscoveryRecord,
        *,
        client_name: str = DEFAULT_CLIENT_NAME,
        connect_timeout: float = DEFAULT_CONNECT_TIMEOUT,
        request_timeout: float = DEFAULT_REQUEST_TIMEOUT,
    ) -> None:
        self.record = record
        self.client_name = client_name
        self.connect_timeout = connect_timeout
        self.request_timeout = request_timeout
        # The hello `capabilities` list of the connected darktable; empty
        # until the first successful handshake, refreshed on reconnect (a
        # restarted darktable may advertise a different set). Callers that
        # gate on a capability must go through `ensure_connected()` first.
        self.capabilities: list[str] = []
        self._conn: _Connection | None = None
        self._lock = asyncio.Lock()

    async def _ensure_connected(self) -> _Connection:
        if self._conn is not None and self._conn.is_connected():
            return self._conn

        conn = _Connection(
            self.record.host,
            self.record.port,
            self.record.token,
            client_name=self.client_name,
            connect_timeout=self.connect_timeout,
            request_timeout=self.request_timeout,
        )
        await conn.connect()
        caps = (conn.server_info or {}).get("capabilities")
        self.capabilities = [c for c in caps if isinstance(c, str)] if isinstance(caps, list) else []
        self._conn = conn
        return conn

    async def ensure_connected(self) -> None:
        """Connects (and completes the hello handshake) if not already
        connected, so `capabilities` is populated. A no-op on a live
        connection."""
        async with self._lock:
            await self._ensure_connected()

    async def call(
        self, method: str, params: dict[str, Any] | None = None, *, timeout: float | None = None
    ) -> dict[str, Any]:
        """Issues one request and returns its `result` object, or raises
        `ProtocolError` (darktable answered with `ok: false`),
        `RequestOutcomeUnknown` (outcome undetermined -- do not retry
        automatically), or `TransportError` (a connection problem
        determined *before* the request could have had any effect,
        e.g. the initial connect was refused).

        Calls are serialized end-to-end by design: `self._lock` is held
        for the whole round trip (connect-if-needed, send, await the
        matching response), so at most one request from this client is
        ever in flight at a time, and concurrent callers queue up rather
        than interleave. This is deliberate, not an oversight -- the
        server enforces its own pending-request limit and today's tool
        set has no mutation whose semantics would benefit from
        interleaved in-flight requests, so genuine concurrency would add
        complexity for no v1 payoff. `_Connection` itself *does* support
        multiple concurrent in-flight requests (it correlates responses
        by id independent of arrival order); a caller that genuinely
        needs concurrent requests to one darktable session must drive a
        `_Connection` directly rather than go through `ProtocolClient`.
        """
        async with self._lock:
            conn = await self._ensure_connected()
            try:
                return await conn.request(method, params, timeout=timeout)
            except (RequestOutcomeUnknown, TransportError):
                # The connection is presumed dead either way; drop it so
                # the *next* call gets a clean reconnect. The exception
                # for *this* call still propagates below, unmodified.
                await conn.close()
                if self._conn is conn:
                    self._conn = None
                raise

    async def close(self) -> None:
        async with self._lock:
            if self._conn is not None:
                await self._conn.close()
                self._conn = None
