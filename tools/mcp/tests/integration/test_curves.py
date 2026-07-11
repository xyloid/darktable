"""Live semantic-curve gates: the milestone-2 rgbcurve acceptance items.

Each test is one acceptance-gate item from the curve-classes design
(§Integration tests) as scoped by the milestone-2 plan: schema/identity
round-trip without native leakage, a real curve patch advancing revision
and visibly changing the preview, scalar+curve+enable landing as one
history item, one undo restoring the exact prior points and mode, the
linked/manual mode transition applying atomically with its documented
channel-0 copy, and a stale ``expected_revision`` leaving state untouched.

Ordering note: these tests share the session-scoped darktable instance and
run in definition order; the identity round-trip runs first because it
asserts the module's pristine default state. Later tests capture the state
they need up front and restore what they change, and same-module history
coalescing (see test_editing.test_undo_round_trip) is broken where it
matters by interposing an exposure edit.
"""

from __future__ import annotations

import base64

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

SEMANTIC_IDS = ("curve.master", "curve.red", "curve.green", "curve.blue")
IDENTITY = [[0.0, 0.0], [1.0, 1.0]]
# The curve design's §Mutation request example patch.
FOUR_POINTS = [[0.0, 0.0], [0.25, 0.18], [0.75, 0.82], [1.0, 1.0]]


def _points(value: dict) -> list[list[float]]:
    return [[p["x"], p["y"]] for p in value["points"]]


def _assert_points_equal(actual: list[list[float]], expected: list[list[float]]) -> None:
    # Stored coordinates are 32-bit floats; compare with a tolerance well
    # above float32 rounding but far below any real curve difference.
    assert len(actual) == len(expected)
    for (ax, ay), (ex, ey) in zip(actual, expected):
        assert ax == pytest.approx(ex, abs=1e-6)
        assert ay == pytest.approx(ey, abs=1e-6)


async def _rgbcurve_params(client) -> dict:
    return await client.call("get_module_params", {"module": "rgbcurve", "instance": 0})


async def _render(client) -> bytes:
    """Renders a small preview after the revision settles, retrying once on
    the documented-transient ``preview_failed`` (trailing history signals
    can advance the revision mid-render right after a mutation)."""
    await harness.wait_for_stable_revision(client)
    for attempt in (1, 2):
        try:
            result = await client.call("render_preview", {"max_px": 256, "quality": 85})
            return base64.b64decode(result["data"])
        except ProtocolError as exc:
            if exc.code != "preview_failed" or attempt == 2:
                raise
    raise AssertionError("unreachable")


async def test_rgbcurve_schema_and_identity_roundtrip(darktable_session):
    """The live schema advertises the four semantic curve IDs with their
    conditions, and the pristine module reads back identity curves --
    with no native layout leaking into the semantic view."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call("get_module_schema", {"module": "rgbcurve"})
        semantic = {f["name"]: f for f in schema["semantic_fields"]}
        assert tuple(f["name"] for f in schema["semantic_fields"]) == SEMANTIC_IDS
        for name in SEMANTIC_IDS:
            assert semantic[name]["class"] == "curve"

        # The native storage fields stay visible but read-only, each
        # annotated with the semantic IDs it backs.
        native = {f["name"]: f for f in schema["fields"]}
        for field in ("curve_nodes", "curve_num_nodes", "curve_type"):
            assert native[field]["writable"] is False
            assert native[field]["represented_by"] == list(SEMANTIC_IDS)

        params = await _rgbcurve_params(client)
        values = params["semantic_values"]
        assert set(values) == set(SEMANTIC_IDS)
        for name in SEMANTIC_IDS:
            _assert_points_equal(_points(values[name]), IDENTITY)
            # No native member names bleed into the semantic value shape.
            assert set(values[name]) == {
                "class", "active", "effective", "writable_now", "points", "interpolation",
            }
        # Linked mode: master is the live curve, per-channel curves are
        # present but inactive (resolved decision 4: never omitted).
        assert values["curve.master"]["active"] is True
        for name in ("curve.red", "curve.green", "curve.blue"):
            assert values[name]["active"] is False
            assert values[name]["writable_now"] is False


async def test_curve_patch_advances_revision_and_preview(darktable_session):
    """A four-point master curve applied with compare-and-swap: read-back
    matches what was written, the revision advances, and the rendered
    preview visibly changes."""
    async with harness.connected_client(darktable_session) as client:
        before = await _render(client)
        rev0 = await harness.wait_for_stable_revision(client)

        result = await client.call(
            "set_module_params",
            {
                "module": "rgbcurve",
                "instance": 0,
                "values": {},
                "semantic_values": {
                    "curve.master": {
                        "class": "curve",
                        "points": [{"x": x, "y": y} for x, y in FOUR_POINTS],
                        "interpolation": "MONOTONE_HERMITE",
                    }
                },
                "expected_revision": rev0,
                "enable": True,
            },
        )
        assert result["revision"] > rev0
        assert result["enabled"] is True
        echoed = result["semantic_values"]["curve.master"]
        _assert_points_equal(_points(echoed), FOUR_POINTS)
        assert echoed["interpolation"] == "MONOTONE_HERMITE"

        # Independent read-back equals the mutation echo.
        params = await _rgbcurve_params(client)
        _assert_points_equal(_points(params["semantic_values"]["curve.master"]), FOUR_POINTS)

        after = await _render(client)
        assert after != before


async def test_scalar_curve_enable_is_one_history_item(darktable_session):
    """A combined scalar + curve + enable patch lands as exactly one new
    history item (the atomic-transaction gate)."""
    async with harness.connected_client(darktable_session) as client:
        # Break same-module coalescing with the previous test's rgbcurve
        # edit, so "exactly one new item" is attributable to this patch.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.5}, "enable": True},
        )
        await harness.wait_for_stable_revision(client)
        history_before = await client.call("get_history", {"limit": 50})

        await client.call(
            "set_module_params",
            {
                "module": "rgbcurve",
                "instance": 0,
                "values": {"compensate_middle_grey": False},
                "semantic_values": {
                    "curve.master": {
                        "class": "curve",
                        "points": [{"x": 0.0, "y": 0.0}, {"x": 0.5, "y": 0.5625}, {"x": 1.0, "y": 1.0}],
                    }
                },
                "enable": True,
            },
        )
        await harness.wait_for_stable_revision(client)
        history_after = await client.call("get_history", {"limit": 50})

        assert len(history_after["items"]) == len(history_before["items"]) + 1
        assert history_after["items"][-1]["op"] == "rgbcurve"


async def test_undo_restores_prior_points_and_mode(darktable_session):
    """One undo of a curve patch restores the exact prior points and
    ``curve_autoscale`` mode."""
    async with harness.connected_client(darktable_session) as client:
        # Separator edit: keep the curve patch below out of any coalesced
        # undo group with earlier rgbcurve items, so one undo == this patch.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.25}},
        )
        await harness.wait_for_stable_revision(client)

        prior = await _rgbcurve_params(client)
        prior_points = _points(prior["semantic_values"]["curve.master"])
        prior_mode = prior["values"]["curve_autoscale"]

        edited = await client.call(
            "set_module_params",
            {
                "module": "rgbcurve",
                "instance": 0,
                "values": {},
                "semantic_values": {
                    "curve.master": {
                        "class": "curve",
                        "points": [{"x": 0.0, "y": 0.125}, {"x": 1.0, "y": 0.875}],
                    }
                },
            },
        )
        _assert_points_equal(
            _points(edited["semantic_values"]["curve.master"]),
            [[0.0, 0.125], [1.0, 0.875]],
        )

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await _rgbcurve_params(client)
        _assert_points_equal(_points(reverted["semantic_values"]["curve.master"]), prior_points)
        assert reverted["values"]["curve_autoscale"] == prior_mode


async def test_manual_mode_transition_atomicity(darktable_session):
    """The linked->manual transition and a per-channel write apply in one
    atomic request: untouched green/blue receive the documented channel-0
    copy, the written red curve lands, and (afterwards, back in linked
    mode) a per-channel write is rejected with ``unsupported_field``
    changing nothing."""
    async with harness.connected_client(darktable_session) as client:
        await harness.wait_for_stable_revision(client)
        before = await _rgbcurve_params(client)
        assert before["values"]["curve_autoscale"] == "DT_S_SCALE_AUTOMATIC_RGB"
        master_points = _points(before["semantic_values"]["curve.master"])

        # Linked mode: writing a per-channel curve violates its
        # writable_when condition and must change nothing.
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "rgbcurve",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "curve.green": {
                            "class": "curve",
                            "points": [{"x": 0.0, "y": 0.25}, {"x": 1.0, "y": 1.0}],
                        }
                    },
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert await _rgbcurve_params(client) == before

        # One request: enter manual RGB and replace curve.red. The
        # transition copies channel 0 into the untouched green/blue
        # curves; the patch then lands on red (which owns channel 0 in
        # manual mode).
        red_points = [[0.0, 0.0], [0.5, 0.375], [1.0, 1.0]]
        result = await client.call(
            "set_module_params",
            {
                "module": "rgbcurve",
                "instance": 0,
                "values": {"curve_autoscale": "DT_S_SCALE_MANUAL_RGB"},
                "semantic_values": {
                    "curve.red": {
                        "class": "curve",
                        "points": [{"x": x, "y": y} for x, y in red_points],
                    }
                },
            },
        )
        assert result["values"]["curve_autoscale"] == "DT_S_SCALE_MANUAL_RGB"
        _assert_points_equal(_points(result["semantic_values"]["curve.red"]), red_points)

        params = await _rgbcurve_params(client)
        values = params["semantic_values"]
        assert values["curve.master"]["active"] is False
        for name in ("curve.red", "curve.green", "curve.blue"):
            assert values[name]["active"] is True
        _assert_points_equal(_points(values["curve.red"]), red_points)
        _assert_points_equal(_points(values["curve.green"]), master_points)
        _assert_points_equal(_points(values["curve.blue"]), master_points)

        # Restore linked mode and an identity master for later tests --
        # also proving conditions are checked against *projected* params:
        # curve.master is writable in the same request that re-enters
        # linked mode.
        await client.call(
            "set_module_params",
            {
                "module": "rgbcurve",
                "instance": 0,
                "values": {"curve_autoscale": "DT_S_SCALE_AUTOMATIC_RGB"},
                "semantic_values": {
                    "curve.master": {
                        "class": "curve",
                        "points": [{"x": x, "y": y} for x, y in IDENTITY],
                    }
                },
            },
        )
        restored = await _rgbcurve_params(client)
        assert restored["values"]["curve_autoscale"] == "DT_S_SCALE_AUTOMATIC_RGB"
        _assert_points_equal(_points(restored["semantic_values"]["curve.master"]), IDENTITY)


async def test_stale_revision_curve_patch_changes_nothing(darktable_session):
    """A curve patch carrying a stale ``expected_revision`` is rejected
    with a retryable ``revision_conflict`` and leaves both scalar and
    semantic state untouched."""
    async with harness.connected_client(darktable_session) as client:
        current = await harness.wait_for_stable_revision(client)
        before = await _rgbcurve_params(client)

        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "rgbcurve",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "curve.master": {
                            "class": "curve",
                            "points": [{"x": 0.0, "y": 1.0}, {"x": 1.0, "y": 0.0}],
                        }
                    },
                    "expected_revision": current + 1000,  # deliberately wrong
                },
            )
        assert excinfo.value.code == "revision_conflict"
        assert excinfo.value.retryable is True

        after = await client.call("get_state")
        assert after["revision"] == current
        assert await _rgbcurve_params(client) == before


@pytest.mark.skip(reason="needs opencl-capable CI runner")
async def test_curve_patch_roundtrip_opencl(darktable_session):
    """The CPU/OpenCL parity variant of the round-trip gate (curve design
    §Integration tests step 8). The shared CI harness runs darktable
    without OpenCL, so this variant is recorded as an explicit skip
    rather than a silent gap."""
