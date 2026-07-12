"""Milestone-3 live gates: tonecurve, colorzones, and basecurve semantic
curves (curve-classes design doc SS Integration tests, as scoped by the
milestone-3 plan).

Each test is one acceptance-gate item for its module: tonecurve's a/b
channel gating on ``tonecurve_autoscale_ab`` (linked vs. manual, mirroring
rgbcurve's own manual-mode gate); colorzones' select-by-driven periodicity
(defaults pulled off the domain edges in hue mode, a zero-wrap-gap curve
rejected only in hue mode, and every output curve reset when the select-by
axis changes -- mirroring the GUI's ``gui_changed()``); and basecurve's
single ``curve.master`` channel round-tripping through a write and an undo.

Ordering note: these tests share the session-scoped darktable instance (see
``conftest.darktable_session``) with ``test_curves.py`` and run after it in
file-collection order. Each test here touches only its own module and reads
that module's pristine defaults before mutating it, then restores what it
changed via undo, so module state never leaks across tests.
"""

from __future__ import annotations

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration


def _points(value: dict) -> list[list[float]]:
    return [[p["x"], p["y"]] for p in value["points"]]


def _assert_points_equal(actual: list[list[float]], expected: list[list[float]]) -> None:
    # Stored coordinates are 32-bit floats; compare with a tolerance well
    # above float32 rounding but far below any real curve difference.
    assert len(actual) == len(expected)
    for (ax, ay), (ex, ey) in zip(actual, expected):
        assert ax == pytest.approx(ex, abs=1e-6)
        assert ay == pytest.approx(ey, abs=1e-6)


def _curve_patch(points: list[list[float]], interpolation: str | None = None) -> dict:
    entry = {"class": "curve", "points": [{"x": x, "y": y} for x, y in points]}
    if interpolation is not None:
        entry["interpolation"] = interpolation
    return entry


async def _schema_ids(client, module: str) -> list[str]:
    schema = await client.call("get_module_schema", {"module": module})
    return [field["name"] for field in schema.get("semantic_fields", [])]


async def _module_params(client, module: str) -> dict:
    return await client.call("get_module_params", {"module": module, "instance": 0})


async def test_tonecurve_schema_and_ab_gating(darktable_session):
    """The live schema advertises L/a/b; the pristine module has a active
    and a/b inactive (default mode is linked automatic RGB); writing a/b
    while linked is rejected and changes nothing; and a single request that
    both flips ``tonecurve_autoscale_ab`` to manual and writes ``curve.a``
    lands atomically (the writable_when condition is checked against the
    *projected* params, so the mode switch in the same request counts)."""
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "tonecurve") == [
            "curve.lightness",
            "curve.a",
            "curve.b",
        ]

        before = await _module_params(client, "tonecurve")
        values = before["semantic_values"]
        assert values["curve.lightness"]["active"] is True
        assert values["curve.a"]["active"] is False  # default mode is linked RGB
        assert values["curve.b"]["active"] is False

        # a-channel write while linked violates writable_when and must
        # change nothing (the whole patch is validated before anything is
        # written).
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "tonecurve",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"curve.a": _curve_patch([[0.0, 0.1], [1.0, 0.9]])},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert await _module_params(client, "tonecurve") == before

        # One request: switch to independent Lab channels and shape a.
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "tonecurve",
                "instance": 0,
                "values": {"tonecurve_autoscale_ab": "DT_S_SCALE_MANUAL"},
                "semantic_values": {
                    "curve.a": _curve_patch(
                        [[0.0, 0.0], [0.5, 0.4], [1.0, 1.0]], "CATMULL_ROM"
                    )
                },
                "expected_revision": revision,
                "enable": True,
            },
        )
        assert result["values"]["tonecurve_autoscale_ab"] == "DT_S_SCALE_MANUAL"
        _assert_points_equal(
            _points(result["semantic_values"]["curve.a"]), [[0.0, 0.0], [0.5, 0.4], [1.0, 1.0]]
        )

        after = await _module_params(client, "tonecurve")
        assert after["semantic_values"]["curve.a"]["active"] is True
        _assert_points_equal(
            _points(after["semantic_values"]["curve.a"]), [[0.0, 0.0], [0.5, 0.4], [1.0, 1.0]]
        )

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > result["revision"]
        reverted = await _module_params(client, "tonecurve")
        assert (
            reverted["values"]["tonecurve_autoscale_ab"]
            == before["values"]["tonecurve_autoscale_ab"]
        )
        assert reverted["semantic_values"]["curve.a"]["active"] is False


async def test_colorzones_periodicity_wrap_and_select_by_reset(darktable_session):
    """The default select-by axis is hue, so ``curve.hue`` (and its
    siblings) read back the periodic defaults pulled inside the domain
    edges; a curve pinned exactly to the edges (zero wrap gap) is rejected
    only in hue mode; a valid periodic patch lands; and a scalar-only
    select-by change resets every output curve to the new axis's defaults,
    mirroring the GUI's ``gui_changed()``. The curve write and the
    select-by change are consecutive edits to the same colorzones instance,
    so darktable's history *list* coalesces them into a single entry (see
    ``test_editing.test_undo_round_trip``) -- but each ``set_module_params``
    call still opens its own undo record (remote edits pass no GUI merge
    target), so undo still unwinds them one at a time: the first undo
    unwinds the select-by change alone (curves stay at the hue-write
    values) and a second undo unwinds the hue write itself, back to the
    pristine defaults.

    splines_version stamping cannot be asserted over the wire: the field is
    write-denylisted (``DENY("splines_version")`` in ``remote_edit.c``) and
    absent from the serialized curve-value JSON. It is covered by the
    C-level unit test
    ``test_colorzones_adapter_write_stamps_splines_version_v2``."""
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "colorzones") == [
            "curve.lightness",
            "curve.chroma",
            "curve.hue",
        ]

        before = await _module_params(client, "colorzones")
        hue_before = before["semantic_values"]["curve.hue"]
        assert [round(p["x"], 4) for p in hue_before["points"]] == [0.25, 0.75]
        assert [round(p["y"], 4) for p in hue_before["points"]] == [0.5, 0.5]

        # Edge-pinned points give a wrap gap of exactly 0, invalid in hue
        # (periodic) mode -- and changes nothing.
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "colorzones",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"curve.hue": _curve_patch([[0.0, 0.4], [1.0, 0.6]])},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        assert excinfo.value.details["constraint"] == "wrap_spacing"
        assert await _module_params(client, "colorzones") == before

        history_before = await client.call("get_history", {"limit": 50})

        # A valid periodic patch lands.
        revision = await harness.wait_for_stable_revision(client)
        patched = await client.call(
            "set_module_params",
            {
                "module": "colorzones",
                "instance": 0,
                "values": {},
                "semantic_values": {"curve.hue": _curve_patch([[0.2, 0.45], [0.8, 0.55]])},
                "expected_revision": revision,
                "enable": True,
            },
        )
        _assert_points_equal(
            _points(patched["semantic_values"]["curve.hue"]), [[0.2, 0.45], [0.8, 0.55]]
        )

        # Scalar-only select-by change resets every curve to the new axis's
        # (non-periodic) defaults, pinned to the domain edges.
        revision = await harness.wait_for_stable_revision(client)
        select_by_result = await client.call(
            "set_module_params",
            {
                "module": "colorzones",
                "instance": 0,
                "values": {"channel": "DT_IOP_COLORZONES_L"},
                "expected_revision": revision,
            },
        )
        after = await _module_params(client, "colorzones")
        hue_after = after["semantic_values"]["curve.hue"]
        assert [round(p["x"], 4) for p in hue_after["points"]] == [0.0, 1.0]
        assert [round(p["y"], 4) for p in hue_after["points"]] == [0.5, 0.5]
        chroma_after = after["semantic_values"]["curve.chroma"]
        assert [round(p["x"], 4) for p in chroma_after["points"]] == [0.0, 1.0]

        # The curve write and the select-by change coalesce into one
        # history-list entry (same module, no interposed edit).
        history_after = await client.call("get_history", {"limit": 50})
        assert len(history_after["items"]) == len(history_before["items"]) + 1
        assert history_after["items"][-1]["op"] == "colorzones"

        # First undo unwinds the select-by change alone: channel and the
        # reset-to-defaults side effect it caused both revert, but the
        # curve write underneath is untouched -- points stay at the
        # hue-write values.
        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > select_by_result["revision"]
        after_first_undo = await _module_params(client, "colorzones")
        assert after_first_undo["values"]["channel"] == before["values"]["channel"]
        _assert_points_equal(
            _points(after_first_undo["semantic_values"]["curve.hue"]), [[0.2, 0.45], [0.8, 0.55]]
        )

        # Second undo unwinds the hue write itself, back to pristine.
        await harness.undo_latest(client)
        restored = await _module_params(client, "colorzones")
        assert restored["values"]["channel"] == before["values"]["channel"]
        _assert_points_equal(
            _points(restored["semantic_values"]["curve.hue"]), _points(hue_before)
        )


async def test_basecurve_master_roundtrip_and_undo(darktable_session):
    """basecurve exposes a single ``curve.master`` semantic channel; a
    written curve reads back exactly, and one undo restores the exact
    prior points."""
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "basecurve") == ["curve.master"]

        before = await _module_params(client, "basecurve")
        prior_points = _points(before["semantic_values"]["curve.master"])

        revision = await harness.wait_for_stable_revision(client)
        written_points = [[0.0, 0.0], [0.25, 0.15], [0.75, 0.85], [1.0, 1.0]]
        result = await client.call(
            "set_module_params",
            {
                "module": "basecurve",
                "instance": 0,
                "values": {},
                "semantic_values": {
                    "curve.master": _curve_patch(written_points, "MONOTONE_HERMITE")
                },
                "expected_revision": revision,
                "enable": True,
            },
        )
        _assert_points_equal(_points(result["semantic_values"]["curve.master"]), written_points)

        # Independent read-back matches the mutation echo.
        params = await _module_params(client, "basecurve")
        _assert_points_equal(_points(params["semantic_values"]["curve.master"]), written_points)

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > result["revision"]
        reverted = await _module_params(client, "basecurve")
        _assert_points_equal(_points(reverted["semantic_values"]["curve.master"]), prior_points)
