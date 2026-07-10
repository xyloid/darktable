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
    """M42: quit while an async ``render_preview`` is still queued/running.
    The stop-drain / reclaim path must let darktable exit cleanly with no
    hang and no leaked pending -- the Task 9 debt."""
    import asyncio

    instance = instance_factory()
    _require_clean_quit(instance)

    async with harness.connected_client(instance) as client:
        # Make sure there is edit state, then queue a large preview and quit
        # almost immediately so the render job is still QUEUED/RUNNING.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 1.5}, "enable": True},
        )
        pending = asyncio.ensure_future(
            client.call("render_preview", {"max_px": 2048, "quality": 92})
        )
        # Let the request reach the server and the job get queued, but do not
        # await completion.
        await asyncio.sleep(0.05)
        pending.cancel()

    # Now quit; this must not hang (bounded wait) and must exit cleanly.
    exit_code = instance.quit_via_dbus(timeout=60)
    assert exit_code == 0, f"darktable did not exit cleanly with a preview in flight ({exit_code})"
    assert not instance.discovery_record_exists()

    log = instance.log_text()
    assert not _CRASH_MARKERS.search(log), "crash marker in darktable shutdown log"
