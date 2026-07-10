"""Live mutation gates: set_module_params (M36), history, and undo.

These formalize the Task 7/8 live-only debts: the mutation path advances the
revision and records history, the Task 8 ``get_history`` clamp fix walks a
real stack, and compare-and-undo round-trips against real darktable state.
"""

from __future__ import annotations

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration


async def test_set_module_params_advances_revision_and_history(darktable_session):
    """M36: edit a real module parameter on a real image; the revision
    advances, the value reads back, and get_history reflects the edit."""
    async with harness.connected_client(darktable_session) as client:
        before = await client.call("get_state")
        rev0 = before["revision"]

        # Apply a concrete, verifiable exposure change and enable the module
        # in the same single history item.
        result = await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {"exposure": 1.25},
                "enable": True,
            },
        )
        assert result["enabled"] is True
        assert result["values"]["exposure"] == pytest.approx(1.25, abs=1e-4)
        assert result["revision"] > rev0

        # Read back from live state, independently of the mutation echo.
        params = await client.call(
            "get_module_params", {"module": "exposure", "instance": 0}
        )
        assert params["values"]["exposure"] == pytest.approx(1.25, abs=1e-4)
        assert params["enabled"] is True

        # get_history live path: the exposure edit is present, ordered.
        history = await client.call("get_history", {"limit": 50})
        assert history["revision"] == result["revision"]
        ops = [item["op"] for item in history["items"]]
        assert "exposure" in ops
        # seq is monotonic non-decreasing (oldest -> newest).
        seqs = [item["seq"] for item in history["items"]]
        assert seqs == sorted(seqs)


async def test_revision_conflict_blocks_stale_mutation(darktable_session):
    """A mutation carrying a stale ``expected_revision`` is rejected with a
    retryable ``revision_conflict`` and changes nothing."""
    async with harness.connected_client(darktable_session) as client:
        state = await client.call("get_state")
        current = state["revision"]
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "exposure",
                    "instance": 0,
                    "values": {"exposure": 0.3},
                    "expected_revision": current + 1000,  # deliberately wrong
                },
            )
        assert excinfo.value.code == "revision_conflict"
        assert excinfo.value.retryable is True
        # Nothing changed.
        after = await client.call("get_state")
        assert after["revision"] == current


async def test_undo_round_trip(darktable_session):
    """Undo live: apply a distinctive exposure edit, compare-and-undo it, and
    confirm the edit is rolled back and undo produced a new revision.

    Asserted invariant is deliberately the *robust* one: after undo the
    exposure value is no longer the value we just set. The exact value undo
    lands on is intentionally NOT asserted, because darktable's undo unwinds
    through its own undo grouping (consecutive same-module parameter edits
    coalesce into one history item, and undo of a coalesced/exposure edit
    reverts the module toward its baseline rather than to the immediately
    preceding numeric value) and the revision jumps by more than one. That
    behaviour is documented as a finding in the task-11 report, not a bug the
    harness should paper over with a lenient tolerance on a specific value."""
    async with harness.connected_client(darktable_session) as client:
        edited = await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": -4.5}, "enable": True},
        )
        assert edited["values"]["exposure"] == pytest.approx(-4.5, abs=1e-4)

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await client.call("get_module_params", {"module": "exposure", "instance": 0})
        # The edit was rolled back: exposure is no longer the value we set.
        assert reverted["values"]["exposure"] != pytest.approx(-4.5, abs=1e-4)
