"""Tests for protocol.py: framing/hello/correlation/timeouts/reconnect,
against the in-process `FakeDarktableServer` fixture from conftest.py.

Several tests build their fake responses directly from the shared C
dispatcher fixtures in `src/tests/unittests/control/fixtures/*.json`
(loaded via `conftest.load_fixture`/`result_handler_from_fixture`), so the
Python client's decoding is checked against the exact same example
payloads the C dispatcher's own tests assert on.
"""

from __future__ import annotations

import asyncio
import json
import struct
from pathlib import Path

import pytest

from conftest import FakeDarktableServer, load_fixture
from darktable_mcp.discovery import DiscoveryRecord
from darktable_mcp.errors import ProtocolError, RequestOutcomeUnknown, TransportError
from darktable_mcp.protocol import MAX_FRAME, ProtocolClient, _Connection, encode_frame


def _record_for(server, **overrides) -> DiscoveryRecord:
    fields = dict(
        path=Path("/nonexistent/session-test.json"),
        protocol="org.darktable.remote-edit",
        protocol_version=1,
        darktable_version="5.x",
        pid=12345,
        host="127.0.0.1",
        port=server.port,
        token=server.token,
        started_at="2026-07-05T15:04:05Z",
    )
    fields.update(overrides)
    return DiscoveryRecord(**fields)


async def test_hello_handshake_success_returns_server_info(fake_server_factory):
    hello_response = load_fixture("hello_response.json")
    server = await fake_server_factory()
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    server.handle("get_state", lambda params: {"view": "lighttable", "image": None, "revision": 0})

    client = ProtocolClient(_record_for(server))
    result = await client.call("get_state")

    assert result == {"view": "lighttable", "image": None, "revision": 0}
    conn = client._conn
    assert conn is not None
    assert conn.server_info == hello_response["result"]

    await client.close()


async def test_hello_wrong_token_raises_protocol_error(fake_server_factory):
    server = await fake_server_factory(token="the-real-token")
    client = ProtocolClient(_record_for(server, token="wrong-token"))

    with pytest.raises(ProtocolError) as excinfo:
        await client.call("get_state")

    assert excinfo.value.code == "unauthorized"
    await client.close()


@pytest.mark.parametrize(
    "method, request_fixture, response_fixture",
    [
        ("get_state", "get_state_request.json", "get_state_response_darkroom.json"),
        ("list_modules", "list_modules_request.json", "list_modules_response.json"),
        (
            "get_module_schema",
            "get_module_schema_request.json",
            "get_module_schema_response.json",
        ),
        (
            "get_module_params",
            "get_module_params_request.json",
            "get_module_params_response.json",
        ),
    ],
)
async def test_success_responses_match_shared_fixtures(
    fake_server_factory, method, request_fixture, response_fixture
):
    request = load_fixture(request_fixture)
    expected_result = load_fixture(response_fixture)["result"]

    server = await fake_server_factory()
    server.handle_from_fixture(method, response_fixture)

    client = ProtocolClient(_record_for(server))
    result = await client.call(method, request.get("params"))

    assert result == expected_result
    await client.close()


@pytest.mark.parametrize(
    "method, params, response_fixture, expected_code",
    [
        (
            "get_module_schema",
            {"module": "nonexistent_op"},
            "get_module_schema_error_unknown_module_response.json",
            "unknown_module",
        ),
        (
            "get_module_params",
            {"module": "nonexistent_op"},
            "get_module_params_error_unknown_module_response.json",
            "unknown_module",
        ),
        (
            "list_modules",
            None,
            "list_modules_error_not_in_darkroom_response.json",
            "not_in_darkroom",
        ),
    ],
)
async def test_error_responses_match_shared_fixtures(
    fake_server_factory, method, params, response_fixture, expected_code
):
    expected_error = load_fixture(response_fixture)["error"]
    server = await fake_server_factory()
    server.handle_from_fixture(method, response_fixture)

    client = ProtocolClient(_record_for(server))
    with pytest.raises(ProtocolError) as excinfo:
        await client.call(method, params)

    assert excinfo.value.code == expected_code == expected_error["code"]
    assert excinfo.value.message == expected_error["message"]
    await client.close()


async def test_unknown_method_error(fake_server_factory):
    expected = load_fixture("error_unknown_method_response.json")
    server = await fake_server_factory()
    # no handler registered for "frobnicate" -> the fake server's default
    # "unknown method" response, which mirrors the fixture's shape.
    client = ProtocolClient(_record_for(server))

    with pytest.raises(ProtocolError) as excinfo:
        await client.call("frobnicate")

    assert excinfo.value.code == expected["error"]["code"]
    assert excinfo.value.message == expected["error"]["message"]
    await client.close()


async def test_protocol_client_serializes_concurrent_calls_correctly(fake_server_factory):
    """`ProtocolClient.call()` holds `self._lock` for the whole round
    trip (see its docstring), so calls issued concurrently via
    `asyncio.gather` are actually serialized end-to-end, never
    interleaved on the wire. This test does NOT exercise id correlation
    of genuinely overlapping in-flight requests -- with the client fully
    serialized, there is never more than one request in flight, so a
    slow handler simply delays the *next* call rather than racing it.
    What this test does verify: results still come back matched to the
    right call even when both are dispatched through `asyncio.gather`
    (i.e. `ProtocolClient` orders/awaits its lock correctly, so slow
    followed by fast, or fast followed by slow, both resolve to their
    own results, never swapped). For a real proof that responses
    arriving out of order are correlated by id, see
    `test_connection_correlates_responses_that_arrive_out_of_order`
    below, which drives `_Connection` directly.
    """
    server = await fake_server_factory()
    server.delay_before_response["get_module_schema"] = 0.15

    def schema_for(params):
        return {"module": params["module"], "fields": [], "slow": True}

    def params_for(params):
        return {"module": params["module"], "fast": True}

    server.handle("get_module_schema", schema_for)
    server.handle("get_module_params", params_for)

    client = ProtocolClient(_record_for(server))

    slow, fast = await asyncio.gather(
        client.call("get_module_schema", {"module": "exposure"}),
        client.call("get_module_params", {"module": "black"}),
    )

    assert slow == {"module": "exposure", "fields": [], "slow": True}
    assert fast == {"module": "black", "fast": True}
    await client.close()


async def test_connection_correlates_responses_that_arrive_out_of_order():
    """The real id-correlation proof the suite was missing: drives
    `_Connection` directly (bypassing `ProtocolClient`'s serializing
    lock -- exactly the escape hatch its docstring points to for genuine
    concurrency), issues two requests that are both in flight at once,
    and has a bare-bones fake peer answer the *second* request before
    the first. If `_Connection._dispatch` correlated by arrival order
    instead of the `id` field, the two results below would come back
    swapped.
    """

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        hello = await FakeDarktableServer._read_frame(reader)
        FakeDarktableServer._write_frame(
            writer,
            {
                "id": hello["id"],
                "ok": True,
                "result": {
                    "protocol_version": 1,
                    "darktable_version": "5.x",
                    "pid": 1,
                    "capabilities": [],
                },
            },
        )
        await writer.drain()

        # Two requests race to be read first; asyncio's scheduling order
        # (and therefore which of `op_first`/`op_second` gets the lower
        # request id) is not guaranteed. Tag each response by the
        # *method* of the request it answers -- read off the request
        # itself, not assumed -- so the assertions below don't depend on
        # which one happened to be sent/read first; what matters is that
        # whichever request arrived *second* gets answered *first*, and
        # each caller still gets back the result for the method it asked
        # for.
        req_a = await FakeDarktableServer._read_frame(reader)
        req_b = await FakeDarktableServer._read_frame(reader)

        # Answer the second-read request first -- the point of the test.
        FakeDarktableServer._write_frame(
            writer, {"id": req_b["id"], "ok": True, "result": {"method": req_b["method"]}}
        )
        await writer.drain()
        FakeDarktableServer._write_frame(
            writer, {"id": req_a["id"], "ok": True, "result": {"method": req_a["method"]}}
        )
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        conn = _Connection("127.0.0.1", port, "tok")
        await conn.connect()

        # Both requests are issued back-to-back, before either response
        # arrives -- genuinely overlapping, unlike anything possible
        # through the serialized `ProtocolClient`.
        first, second = await asyncio.gather(
            conn.request("op_first", {}),
            conn.request("op_second", {}),
        )

        assert first == {"method": "op_first"}
        assert second == {"method": "op_second"}
        await conn.close()
    finally:
        server.close()
        await server.wait_closed()


async def test_request_timeout_raises_request_outcome_unknown(fake_server_factory):
    server = await fake_server_factory()
    server.drop_requests.add("list_modules")  # darktable never answers this one

    client = ProtocolClient(_record_for(server), request_timeout=0.1)

    with pytest.raises(RequestOutcomeUnknown):
        await client.call("list_modules")

    await client.close()


async def test_reconnects_automatically_after_timeout(fake_server_factory):
    server = await fake_server_factory()
    server.drop_requests.add("list_modules")
    server.handle("get_state", lambda params: {"view": "lighttable", "image": None, "revision": 0})

    client = ProtocolClient(_record_for(server), request_timeout=0.1)

    with pytest.raises(RequestOutcomeUnknown):
        await client.call("list_modules")

    assert server.connections_seen == 1

    # The dropped request's connection is presumed dead; the next call
    # must open a fresh one rather than reusing the stuck socket.
    result = await client.call("get_state")

    assert result == {"view": "lighttable", "image": None, "revision": 0}
    assert server.connections_seen == 2
    await client.close()


async def test_reconnects_after_clean_disconnect_between_calls(fake_server_factory):
    server = await fake_server_factory()
    server.handle("get_state", lambda params: {"view": "lighttable", "image": None, "revision": 0})

    client = ProtocolClient(_record_for(server))
    await client.call("get_state")
    assert server.connections_seen == 1

    # Simulate darktable (or an intermediary) closing the idle connection
    # between calls -- no request was in flight, so this is unambiguous.
    await client._conn.close()

    result = await client.call("get_state")

    assert result == {"view": "lighttable", "image": None, "revision": 0}
    assert server.connections_seen == 2
    await client.close()


async def test_connection_refused_raises_transport_error():
    # Nothing is listening on this port (it was just released by the OS
    # after the probe below, so binding it ourselves is unnecessary --
    # picking an arbitrary high port with nothing bound is sufficient for
    # "refused" on loopback).
    record = DiscoveryRecord(
        path=Path("/nonexistent/session-test.json"),
        protocol="org.darktable.remote-edit",
        protocol_version=1,
        darktable_version="5.x",
        pid=1,
        host="127.0.0.1",
        port=1,  # privileged/unused port: connection refused on any normal system
        token="tok",
        started_at="2026-07-05T15:04:05Z",
    )
    client = ProtocolClient(record, connect_timeout=1.0)

    with pytest.raises(TransportError):
        await client.call("get_state")


def test_encode_frame_rejects_oversized_payload():
    huge = {"id": 1, "method": "x", "params": {"blob": "a" * (17 * 1024 * 1024)}}
    with pytest.raises(TransportError):
        encode_frame(huge)


async def _write_hello_ok(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    """Reads the mandatory first `hello` frame and answers it with a
    plain success response, for the hand-rolled fake peers below that
    need full control over what happens on the *next* frame (the
    `FakeDarktableServer` fixture in conftest.py always speaks a
    well-formed protocol, so these adversarial cases -- a lying length
    header, a mid-frame disconnect, a bogus response id -- are written
    directly against a bare `asyncio.start_server` instead).
    """
    hello = await FakeDarktableServer._read_frame(reader)
    FakeDarktableServer._write_frame(
        writer,
        {
            "id": hello["id"],
            "ok": True,
            "result": {
                "protocol_version": 1,
                "darktable_version": "5.x",
                "pid": 1,
                "capabilities": [],
            },
        },
    )
    await writer.drain()


async def test_oversize_frame_header_rejected_without_reading_body():
    """A header declaring a frame larger than the 16 MiB cap must be
    rejected as soon as the header itself is parsed -- before the read
    loop ever calls `readexactly(length)` for the body. A peer that lies
    about the length and then sends nothing further would hang a client
    that buffered/waited for the body first and validated the cap only
    afterwards; bounding this test with `asyncio.wait_for` turns that
    failure mode into a prompt, readable assertion failure instead of a
    hung test suite.
    """

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await _write_hello_ok(reader, writer)
        await FakeDarktableServer._read_frame(reader)  # the real request
        # Oversize header, then nothing: no body is ever sent, and the
        # connection is deliberately NOT closed yet -- if it were, a
        # buggy client that buffered-the-body-then-checked-the-cap would
        # fail with a mid-frame EOF (also a TransportError) and this
        # test could not tell the two behaviors apart. Keeping the
        # socket open means such a client would *hang* waiting for
        # ~16 MiB that never comes, which the 2-second bound below turns
        # into a TimeoutError (not a TransportError) -> test failure.
        writer.write(struct.pack(">I", MAX_FRAME + 1))
        await writer.drain()
        # Wait for the client to give up and close, then release the
        # server-side transport. This close is teardown-only: it cannot
        # happen until after the client-side assertion has resolved.
        # (Needed because `StreamReaderProtocol.eof_received` keeps the
        # transport open on client EOF, and `Server.wait_closed()` on
        # Python >= 3.12.1 waits for every accepted connection to close;
        # a handler that returns without closing would hang the suite.)
        await reader.read()
        writer.close()

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        conn = _Connection("127.0.0.1", port, "tok")
        await conn.connect()

        # The 2-second bound is the "promptly" assertion: the client must
        # fail as soon as it parses the lying header, long before any
        # body bytes could have been waited for or buffered.
        with pytest.raises(TransportError):
            await asyncio.wait_for(conn.request("get_state", {}), timeout=2.0)

        await conn.close()  # teardown: lets the fake peer see EOF and exit
    finally:
        server.close()
        await server.wait_closed()


async def test_truncated_frame_body_surfaces_as_ambiguous_outcome():
    """A valid header followed by a connection close partway through the
    body must not look like a clean, well-formed disconnect: `readexactly`
    raises `IncompleteReadError` with a non-empty `.partial`, and the read
    loop must surface that as a mid-frame-disconnect `TransportError`,
    which `_Connection._request`/`ProtocolClient.call` turn into
    `RequestOutcomeUnknown` for any request that was in flight -- never a
    silent/clean EOF, and never an attempt to `json.loads` the partial
    bytes.
    """

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await _write_hello_ok(reader, writer)
        await FakeDarktableServer._read_frame(reader)  # the real request
        body = json.dumps({"id": 2, "ok": True, "result": {}}).encode("utf-8")
        writer.write(struct.pack(">I", len(body)))
        writer.write(body[: len(body) // 2])  # half the body...
        await writer.drain()
        writer.close()  # ...then disconnect mid-frame.

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        conn = _Connection("127.0.0.1", port, "tok")
        await conn.connect()

        with pytest.raises(RequestOutcomeUnknown):
            await asyncio.wait_for(conn.request("get_state", {}), timeout=2.0)

        await conn.close()  # teardown only; the connection is already dead
    finally:
        server.close()
        await server.wait_closed()


async def test_unknown_id_response_is_ignored_and_real_request_still_completes():
    """A response frame whose `id` was never requested (e.g. a stray or
    delayed response tied to some earlier, already-abandoned request)
    must be silently dropped by `_Connection._dispatch` rather than
    crashing the read loop or being delivered to the wrong caller; the
    next, correctly-addressed response must still complete the real
    request normally.
    """

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await _write_hello_ok(reader, writer)
        req = await FakeDarktableServer._read_frame(reader)
        # A bogus response for an id that was never requested...
        FakeDarktableServer._write_frame(writer, {"id": 999999, "ok": True, "result": {"bogus": True}})
        await writer.drain()
        # ...followed by the real response.
        FakeDarktableServer._write_frame(writer, {"id": req["id"], "ok": True, "result": {"real": True}})
        await writer.drain()
        writer.close()  # both frames are already queued; TCP delivers them before the FIN

    server = await asyncio.start_server(handle, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    try:
        conn = _Connection("127.0.0.1", port, "tok")
        await conn.connect()

        result = await asyncio.wait_for(conn.request("get_state", {}), timeout=2.0)
        assert result == {"real": True}
        await conn.close()
    finally:
        server.close()
        await server.wait_closed()


def test_encode_frame_header_is_big_endian_length_prefix():
    frame = encode_frame({"id": 1, "method": "get_state"})
    body = frame[4:]
    header_len = int.from_bytes(frame[:4], "big")
    assert header_len == len(body)


def test_connection_and_client_reprs_do_not_leak_token():
    """`_Connection` and `ProtocolClient` are plain classes whose default
    `object.__repr__` shows no attributes, so the session token they hold
    cannot leak through a stray log line today. This test pins that down
    (mirroring `DiscoveryRecord`'s explicit `repr=False` on `token`) so a
    future conversion to `@dataclass` -- whose generated repr would print
    every field -- fails loudly here instead of silently starting to leak.
    """
    secret = "s3cr3t-token-do-not-leak"
    conn = _Connection("127.0.0.1", 43127, secret)
    record = DiscoveryRecord(
        path=Path("/tmp/session-1.json"),
        protocol="org.darktable.remote-edit",
        protocol_version=1,
        darktable_version="5.x",
        pid=12345,
        host="127.0.0.1",
        port=43127,
        token=secret,
        started_at="2026-07-05T15:04:05Z",
    )
    client = ProtocolClient(record)

    for obj in (conn, client):
        assert secret not in repr(obj)
        assert secret not in str(obj)
