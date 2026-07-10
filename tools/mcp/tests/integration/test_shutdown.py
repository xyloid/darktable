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

    Trigger: after establishing edit state, fire a *burst* of expensive
    ``render_preview`` jobs (plus a ``compute_scopes``) back-to-back without
    awaiting any of them, then quit immediately -- no settle sleep. The async
    render queue admits multiple jobs, so even if the first has started, the
    remainder sit QUEUED and are reclaimed by the stop-time walk at quit,
    exercising the drain/reclaim path directly rather than relying on a
    single job still being mid-render after a fixed delay (the old, weak
    trigger: one 2048 px render of a 256x256 image could finish inside the
    0.05 s sleep, leaving nothing to drain and letting a regression pass
    unnoticed).

    LIMITATION -- what this test does and does NOT pin (cannot be made
    airtight from the client side): job scheduling and render duration live
    entirely inside darktable, and there is no client-visible "job started"
    signal to synchronise on, so this test cannot *guarantee* a job is
    mid-render at the exact instant ``dt_control_quit()`` runs its
    stop-drain. What it deterministically pins is the invariant that
    matters: submitting a burst of render/scope work and then quitting must
    ALWAYS yield a clean, bounded exit (code 0), a removed discovery record,
    and no crash marker -- whether that work was running, still queued, or
    already drained at quit. Because the burst guarantees work was actually
    submitted to the server, the assertions cannot pass vacuously on "an
    empty queue"; a stop-drain/reclaim regression that hung or crashed on
    queued work would fail here. Pinning the guaranteed-mid-render path would
    require instrumenting the C server, which this suite may not modify.
    """
    import asyncio

    instance = instance_factory()
    _require_clean_quit(instance)

    async with harness.connected_client(instance) as client:
        # Make sure there is real edit state so each render has work to do.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 1.5}, "enable": True},
        )
        # Fire a burst of async work without awaiting completion: several
        # large previews plus a heavy scope compute. Enough jobs that the
        # queue cannot plausibly have drained them all before quit fires.
        pending = [
            asyncio.ensure_future(
                client.call("render_preview", {"max_px": 2048, "quality": 92})
            )
            for _ in range(6)
        ]
        pending.append(
            asyncio.ensure_future(
                client.call(
                    "compute_scopes",
                    {
                        "scopes": ["histogram", "waveform"],
                        "include_summary": True,
                        "include_bins": True,
                        "include_images": True,
                        "image_size": 256,
                    },
                )
            )
        )
        # Yield exactly once so every request's send half runs (frame written
        # to the socket), but do NOT wait for any render to complete -- there
        # is deliberately no settle sleep here.
        await asyncio.sleep(0)
        for fut in pending:
            fut.cancel()

    # Now quit; this must not hang (bounded wait) and must exit cleanly.
    exit_code = instance.quit_via_dbus(timeout=60)
    assert exit_code == 0, f"darktable did not exit cleanly with previews in flight ({exit_code})"
    assert not instance.discovery_record_exists()

    log = instance.log_text()
    assert not _CRASH_MARKERS.search(log), "crash marker in darktable shutdown log"
