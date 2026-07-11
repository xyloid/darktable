"""Unit tests for deterministic helpers in the live integration harness."""

from __future__ import annotations

from integration import harness


async def test_wait_for_stable_revision_ignores_one_quiet_interval():
    class DelayedRevisionClient:
        def __init__(self) -> None:
            self.revisions = iter((17, 17, 18, 18, 18))
            self.calls = 0

        async def call(self, method: str) -> dict[str, int]:
            assert method == "get_state"
            self.calls += 1
            return {"revision": next(self.revisions)}

    client = DelayedRevisionClient()
    revision = await harness.wait_for_stable_revision(
        client, consecutive_reads=3, poll_interval=0, timeout=1
    )

    assert revision == 18
    assert client.calls == 5
