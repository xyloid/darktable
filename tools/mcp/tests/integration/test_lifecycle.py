"""Discovery, handshake, and security-property gates against live darktable.

Covers the plan's "discovery lifecycle" debt (record appears with 0600
perms, correct PID/liveness) and the standing token-redaction constraint.
Clean-up-on-exit is asserted by the shutdown gates in ``test_shutdown.py``.
"""

from __future__ import annotations

import os
import stat

import pytest

from darktable_mcp import discovery

from . import harness

pytestmark = pytest.mark.integration


def test_discovery_record_present_and_locked_down(darktable_session):
    """The record exists, is 0600, names this process, and that PID is
    alive -- the discovery-lifecycle debt from Task 4/plan step D."""
    record = darktable_session.record
    path = record.path
    assert path.exists()

    mode = stat.S_IMODE(os.stat(path).st_mode)
    # user-only rw; no group/other bits. (Enforced on POSIX; on Windows the
    # writer relies on the default ACL + the mandatory token instead --
    # audited in the report's cross-platform checklist.)
    assert mode == 0o600, f"discovery record perms {oct(mode)} != 0o600"

    assert record.pid == darktable_session.pid
    assert record.protocol == discovery.EXPECTED_PROTOCOL
    assert record.protocol_version == 1
    assert record.host == "127.0.0.1"
    assert 0 < record.port <= 65535
    assert discovery.is_process_alive(record.pid)
    assert record.token, "record must carry a session token"


async def test_hello_handshake_reports_matching_pid(darktable_session):
    """A ``hello`` on the discovered port authenticates and the server
    reports the same PID discovery selected -- proving the sidecar reached
    the intended instance."""
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info  # populated by the hello handshake
        assert info is not None
        assert info["protocol_version"] == 1
        assert info["pid"] == darktable_session.pid
        assert "params" in info["capabilities"]


async def test_get_state_is_darkroom_with_image(darktable_session):
    """The harness opens the test image directly in the darkroom, so the
    darkroom-scoped tools have something to act on."""
    async with harness.connected_client(darktable_session) as client:
        state = await client.call("get_state")
    assert state["view"] == "darkroom"
    assert state["image"] is not None
    assert state["image"]["filename"] == "test.png"
    assert state["image"]["width"] == 256 and state["image"]["height"] == 256
    assert isinstance(state["revision"], int)


def test_captured_log_never_contains_the_token(darktable_session):
    """Standing security gate: the token darktable generated must not appear
    in its own captured stderr, and the harness' redaction blanks it even if
    it ever did."""
    token = darktable_session.record.token
    # Raw log on disk: darktable must not print its own token.
    raw = darktable_session.log_path.read_text(encoding="utf-8", errors="replace")
    assert token not in raw, "darktable leaked the session token into its log"
    # The harness accessor is additionally redaction-safe.
    harness.assert_token_absent(darktable_session.log_text(), token)


def test_unauthorized_first_frame_is_rejected(darktable_session):
    """A client that does not present the token cannot drive the server: a
    non-``hello`` first frame (or a bad token) is refused. Exercised via a
    fresh raw connection so it doesn't disturb the shared client."""
    import asyncio
    import json
    import struct

    record = darktable_session.record

    async def _probe() -> dict:
        reader, writer = await asyncio.open_connection(record.host, record.port)
        try:
            # Send a non-hello method as the very first frame.
            body = json.dumps({"id": 1, "method": "get_state"}).encode()
            writer.write(struct.pack(">I", len(body)) + body)
            await writer.drain()
            header = await asyncio.wait_for(reader.readexactly(4), timeout=10)
            (length,) = struct.unpack(">I", header)
            payload = await asyncio.wait_for(reader.readexactly(length), timeout=10)
            return json.loads(payload)
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    response = asyncio.new_event_loop().run_until_complete(_probe())
    assert response["ok"] is False
    assert response["error"]["code"] == "unauthorized"
