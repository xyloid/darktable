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
from pathlib import Path

import pytest

from conftest import load_fixture
from darktable_mcp.discovery import DiscoveryRecord
from darktable_mcp.errors import ProtocolError, RequestOutcomeUnknown, TransportError
from darktable_mcp.protocol import ProtocolClient, encode_frame


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


async def test_request_id_correlation_survives_out_of_order_responses(fake_server_factory):
    server = await fake_server_factory()
    # exposure's handler is slow; black's is fast -- if the client mixed
    # up ids, one of these two assertions below would get the other
    # method's result instead.
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


def test_encode_frame_header_is_big_endian_length_prefix():
    frame = encode_frame({"id": 1, "method": "get_state"})
    body = frame[4:]
    header_len = int.from_bytes(frame[:4], "big")
    assert header_len == len(body)
