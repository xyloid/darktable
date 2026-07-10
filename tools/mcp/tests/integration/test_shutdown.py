"""Shutdown gates: clean exit + discovery cleanup (M33) and quit while an
async preview is in flight (M42).

Both need a *normal* quit, which for darktable means its D-Bus
``Remote.Quit`` method (the same ``dt_control_quit()`` path as closing the
window); SIGTERM would be an abrupt kill that neither cleans up the
discovery record nor exercises the stop-drain path. When a private D-Bus
session bus + gdbus is unavailable these gates skip loudly rather than
assert on a kill.

Each test drives its own instance (via ``instance_factory``) because it
shuts that instance down; the shared ``darktable_session`` is left untouched.
"""

from __future__ import annotations

import re

import pytest

from . import harness

pytestmark = pytest.mark.integration

# darktable's own crash paths log these before/while dumping a backtrace.
_CRASH_MARKERS = re.compile(r"segmentation fault|sigsegv|backtrace|g_assert|assertion.*failed|abort",
                            re.IGNORECASE)


def _require_clean_quit(instance: harness.DarktableInstance) -> None:
    if not instance.can_quit_cleanly():
        pytest.skip(
            "clean-quit gate needs a private D-Bus session bus and gdbus "
            "(install dbus-daemon + gdbus); refusing to fake it with SIGTERM"
        )


async def test_clean_exit_after_mutation_session(instance_factory):
    """M33 shutdown-segfault watch: after a real mutation session, a normal
    quit exits with code 0, removes the discovery record, and logs no crash
    marker."""
    instance = instance_factory()
    _require_clean_quit(instance)

    async with harness.connected_client(instance) as client:
        # A representative mutation session (this is the state that the one
        # unattributed Task-7 segfault was seen after): a parameter edit, an
        # enable toggle, and an undo.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 1.0}, "enable": True},
        )
        await client.call("set_module_enabled", {"module": "exposure", "instance": 0, "enabled": False})
        await harness.undo_latest(client)

    exit_code = instance.quit_via_dbus(timeout=60)
    assert exit_code == 0, f"darktable exited non-zero ({exit_code}) on normal quit"
    assert not instance.discovery_record_exists(), "discovery record not cleaned up on exit"

    log = instance.log_text()
    assert not _CRASH_MARKERS.search(log), "crash marker in darktable shutdown log"


async def test_quit_with_inflight_preview_does_not_hang(instance_factory):
    """M42: quit while async render/scope work is still queued or running.
    The stop-drain / reclaim path must let darktable exit cleanly with no
    hang and no leaked pending -- the Task 9 debt.

    Trigger: after establishing edit state, fire a burst of seven async
    requests (six expensive ``render_preview`` jobs plus a
    ``compute_scopes``) as genuinely concurrent in-flight requests, then
    quit with no settle sleep. The burst must drive ``protocol._Connection``
    directly: ``ProtocolClient.call()`` holds a lock for the whole round
    trip (at most ONE request from a client is ever in flight, per its own
    docstring), so a burst through the client would serialize down to a
    single sent frame -- the same weak single-job trigger this rewrite
    replaces. ``_Connection`` is the documented escape hatch ("a caller
    that genuinely needs concurrent requests to one darktable session must
    drive a _Connection directly"): its ``_request`` writes each frame
    before awaiting the response, and it correlates responses by id.

    That the burst genuinely reached the server is asserted, not assumed,
    on two levels:

    * client side: every burst request id is registered in-flight (its
      frame written to the transport) before the test proceeds;
    * server side: a trailing synchronous ``get_state`` on the SAME
      connection is awaited to completion. All frames travel one TCP
      stream and the server reads and dispatches them strictly in order
      (``_on_frame_cb`` -> ``_handle_ready_frame`` in remote_server.c), so
      the ``get_state`` response proves darktable received and dispatched
      all seven earlier frames first. Both burst methods are async
      handlers that defer into server-side pendings (``is_async`` in
      remote_protocol.c's method table), and seven is under
      DT_REMOTE_SERVER_MAX_PENDING (8), so none is busy-rejected. The test
      further asserts at least one burst request is still unanswered at
      that point, i.e. real pendings existed and the work had NOT already
      drained when quit fires.

    LIMITATION -- what this test does and does NOT pin: there is no
    client-visible "job started"/"job still queued" signal, so the exact
    stop-time split between RUNNING jobs (drained) and QUEUED jobs
    (reclaimed by the stop-time walk) cannot be controlled from the client
    side; pinning a guaranteed-mid-render instant would require
    instrumenting the C server, which this suite may not modify. What IS
    pinned deterministically: seven async jobs were verifiably admitted
    server-side and at least one was still pending when quit fired, so the
    stop-drain/reclaim path has real work to process and the assertions
    (bounded exit 0, discovery record removed, no crash marker) cannot
    pass vacuously on an empty queue.
    """
    import asyncio

    from darktable_mcp.protocol import _Connection

    instance = instance_factory()
    _require_clean_quit(instance)

    record = instance.record
    conn = _Connection(
        record.host, record.port, record.token, connect_timeout=15.0, request_timeout=90.0
    )
    await conn.connect()  # hello handshake
    try:
        # Make sure there is real edit state so each render has work to do.
        await conn.request(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 1.5}, "enable": True},
        )

        # Fire the burst without awaiting completion. Each task's first
        # step runs synchronously through _request up to its frame write,
        # so a single sleep(0) lets every frame reach the transport.
        burst = [
            asyncio.ensure_future(
                conn.request("render_preview", {"max_px": 2048, "quality": 92})
            )
            for _ in range(6)
        ]
        burst.append(
            asyncio.ensure_future(
                conn.request(
                    "compute_scopes",
                    {
                        "scopes": ["histogram"],
                        "include_summary": True,
                        "include_bins": True,
                        "include_images": True,
                        "image_size": 256,
                    },
                )
            )
        )
        await asyncio.sleep(0)
        assert len(conn._pending) >= len(burst), (
            f"only {len(conn._pending)} of {len(burst)} burst frames were written"
        )

        # Server-side receipt proof: this synchronous request was framed
        # AFTER the whole burst on the same TCP stream, so its response
        # means the server already read and dispatched all seven burst
        # frames (deferring each into a pending).
        await conn.request("get_state")

        still_pending = [fut for fut in burst if not fut.done()]
        assert still_pending, (
            "all burst jobs already completed before quit could fire; "
            "the stop-drain path would not be exercised"
        )

        # Abandon the in-flight requests client-side (their frames are on
        # the wire and admitted server-side); reap the tasks quietly.
        for fut in burst:
            fut.cancel()
        await asyncio.gather(*burst, return_exceptions=True)
    finally:
        await conn.close()

    # Now quit, with jobs verifiably still pending server-side; this must
    # not hang (bounded wait) and must exit cleanly.
    exit_code = instance.quit_via_dbus(timeout=60)
    assert exit_code == 0, f"darktable did not exit cleanly with previews in flight ({exit_code})"
    assert not instance.discovery_record_exists()

    log = instance.log_text()
    assert not _CRASH_MARKERS.search(log), "crash marker in darktable shutdown log"
