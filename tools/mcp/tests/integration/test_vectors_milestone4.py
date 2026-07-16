"""Milestone-4 live gates: the five semantic-vector adapters (colorbalance,
channelmixerrgb, rgblevels, borders, watermark), patterned on
``test_curves_milestone3.py`` -- session fixtures, the render-diff helper,
history-length assertions, undo (``test_curves.py:178``), and stale-revision
(``test_curves.py:307``) all mirror that file's own shapes, adapted from
curve to vector wire entries (``{"class": "vector", "values": [...]}``
under ``semantic_values``, matching the raw wire protocol these
``harness``-based tests speak directly -- the ``vectors=`` sugar on
``set_module_params`` is a FastMCP-tool-layer convenience this suite does
not go through, exactly as the curve gates never go through the tool
layer's ``curves=`` sugar).

Error-shape binding (per the milestone-4 vector-class design's adjudicated
behavior): a write to a semantic name whose ``writable_when`` predicate is
not satisfied returns ``unsupported_field`` (the same shape the curve gates
assert for their own linked/manual and hue/non-hue gating -- see
``test_curves.py:250`` and ``test_curves_milestone3.py:95``), not
``invalid_value``. Ordering violations (rgblevels) and the
divide-by-zero guard (channelmixerrgb) are genuinely ``invalid_value``.

Gate 8 (curve regression) needs no new code here: running this file under
``pytest -m integration`` in the same invocation as ``test_curves.py`` and
``test_curves_milestone3.py`` (all three live in this package, all marked
``integration``) exercises every existing curve gate alongside these new
vector gates in one session.

KNOWN BLOCKER (discovered writing this file, documented in the task-10
report): every write-path gate below (all except the two synchronous
``test_watermark_params_v7_struct_size`` checks) currently fails, not from
a defect in this test file, but from a real dispatch bug in
``dt_remote_curve_apply_patch()`` (``src/control/remote_curve.c``): its
"no curve adapter for this op" branch tests the *class-generic*
``has_semantics`` flag (any semantic entry present) rather than a
curve-scoped one, so it rejects with ``unsupported_field`` before
``dt_remote_vector_apply_patch()`` (``src/control/remote_vector.c``) --
which itself correctly scopes the same check to ``has_vector_semantics``,
lines ~409-420/435-444 -- is ever reached. None of the five vector-only
modules under test here register a curve adapter, so this fires on every
``set_module_params`` call carrying a vector ``semantic_values`` entry to
any of them, confirmed by reproducing it live against all five ops. Left
unpatched per the task's explicit instruction not to touch engine code;
see the report for the full repro and suggested fix shape.
"""

from __future__ import annotations

import base64
import struct

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration


async def _module_params(client, module: str, instance: int = 0) -> dict:
    return await client.call("get_module_params", {"module": module, "instance": instance})


async def _render(client) -> bytes:
    """Renders a small preview after the revision settles, retrying once on
    the documented-transient ``preview_failed`` -- identical to
    ``test_curves.py``'s own ``_render()``."""
    await harness.wait_for_stable_revision(client)
    for attempt in (1, 2):
        try:
            result = await client.call("render_preview", {"max_px": 256, "quality": 85})
            return base64.b64decode(result["data"])
        except ProtocolError as exc:
            if exc.code != "preview_failed" or attempt == 2:
                raise
    raise AssertionError("unreachable")


def _vec(values: list[float]) -> dict:
    return {"class": "vector", "values": values}


def _assert_values_equal(actual: list[float], expected: list[float], *, abs_tol: float = 1e-6) -> None:
    # Stored components are 32-bit floats; compare with a tolerance well
    # above float32 rounding but far below any real difference -- same
    # convention as the curve gates' _assert_points_equal.
    assert len(actual) == len(expected)
    for a, e in zip(actual, expected):
        assert a == pytest.approx(e, abs=abs_tol)


# ---------------------------------------------------------------------------
# 1. colorbalance: mode-gated lift/gamma/gain <-> offset/power/slope aliases
# ---------------------------------------------------------------------------


async def test_colorbalance_sop_writes_and_lift_gating(darktable_session):
    """SLOPE_OFFSET_POWER (SOP) is the module default: 'lift' is gated off
    (writable_when: mode != SLOPE_OFFSET_POWER) and rejected with
    unsupported_field, changing nothing; offset/power/slope round-trip
    with a visible render-diff; a single request that both switches mode
    to LIFT_GAMMA_GAIN and writes 'lift' lands atomically (writable_when
    is checked against the *projected* params, the tonecurve/colorzones
    precedent)."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "colorbalance")
        assert before["values"]["mode"] == "SLOPE_OFFSET_POWER"

        history_before = await client.call("get_history", {"limit": 50})
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "colorbalance",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"lift": _vec([1.0, 1.1, 1.0, 0.95])},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert await _module_params(client, "colorbalance") == before
        history_after_reject = await client.call("get_history", {"limit": 50})
        assert len(history_after_reject["items"]) == len(history_before["items"])

        before_render = await _render(client)

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalance",
                "instance": 0,
                "values": {},
                "semantic_values": {
                    "offset": _vec([1.1, 1.05, 0.95, 1.0]),
                    "power": _vec([0.9, 1.0, 1.0, 1.1]),
                    "slope": _vec([1.05, 0.9, 1.1, 1.0]),
                },
                "expected_revision": revision,
                "enable": True,
            },
        )
        _assert_values_equal(result["semantic_values"]["offset"]["values"], [1.1, 1.05, 0.95, 1.0])
        _assert_values_equal(result["semantic_values"]["power"]["values"], [0.9, 1.0, 1.0, 1.1])
        _assert_values_equal(result["semantic_values"]["slope"]["values"], [1.05, 0.9, 1.1, 1.0])

        params = await _module_params(client, "colorbalance")
        _assert_values_equal(params["semantic_values"]["offset"]["values"], [1.1, 1.05, 0.95, 1.0])
        _assert_values_equal(params["semantic_values"]["power"]["values"], [0.9, 1.0, 1.0, 1.1])
        _assert_values_equal(params["semantic_values"]["slope"]["values"], [1.05, 0.9, 1.1, 1.0])

        after_render = await _render(client)
        assert after_render != before_render

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalance",
                "instance": 0,
                "values": {"mode": "LIFT_GAMMA_GAIN"},
                "semantic_values": {"lift": _vec([1.0, 1.1, 1.0, 0.95])},
                "expected_revision": revision,
            },
        )
        assert result["values"]["mode"] == "LIFT_GAMMA_GAIN"
        _assert_values_equal(result["semantic_values"]["lift"]["values"], [1.0, 1.1, 1.0, 0.95])

        params = await _module_params(client, "colorbalance")
        assert params["semantic_values"]["lift"]["active"] is True
        _assert_values_equal(params["semantic_values"]["lift"]["values"], [1.0, 1.1, 1.0, 0.95])
        assert params["semantic_values"]["offset"]["active"] is False


# ---------------------------------------------------------------------------
# 2. channelmixerrgb: independent rows + the red/green/blue zero-sum guard
# ---------------------------------------------------------------------------


async def test_channelmixerrgb_red_row_write_and_zero_sum_guard(darktable_session):
    """'red' round-trips with a visible render-diff; a normalize-enabled
    row that narrows to a sum of exactly zero (a real divide-by-zero in
    commit_params(), which has no fallback for red/green/blue unlike
    grey's own guard) is rejected with invalid_value, changing nothing."""
    async with harness.connected_client(darktable_session) as client:
        before_render = await _render(client)

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "channelmixerrgb",
                "instance": 0,
                "values": {},
                "semantic_values": {"red": _vec([0.8, 0.1, 0.1])},
                "expected_revision": revision,
            },
        )
        _assert_values_equal(result["semantic_values"]["red"]["values"], [0.8, 0.1, 0.1])
        params = await _module_params(client, "channelmixerrgb")
        _assert_values_equal(params["semantic_values"]["red"]["values"], [0.8, 0.1, 0.1])

        after_render = await _render(client)
        assert after_render != before_render

        guarded_before = await _module_params(client, "channelmixerrgb")
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "channelmixerrgb",
                    "instance": 0,
                    "values": {"normalize_R": True},
                    "semantic_values": {"red": _vec([1.0, -1.0, 0.0])},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        assert await _module_params(client, "channelmixerrgb") == guarded_before


# ---------------------------------------------------------------------------
# 3. rgblevels: autoscale-gated linked/independent aliases, no reset, order
# ---------------------------------------------------------------------------


async def test_rgblevels_independent_channels_write_and_ordering(darktable_session):
    """One request switches autoscale to INDEPENDENT_CHANNELS and writes
    'levels.red'; green/blue rows read back at their pristine defaults (the
    adapter never resets a row on an autoscale switch); a visible
    render-diff; a non-strictly-increasing triple is rejected with
    invalid_value."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "rgblevels")
        assert before["values"]["autoscale"] == "DT_IOP_RGBLEVELS_LINKED_CHANNELS"

        before_render = await _render(client)

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "rgblevels",
                "instance": 0,
                "values": {"autoscale": "DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS"},
                "semantic_values": {"levels.red": _vec([0.05, 0.4, 0.95])},
                "expected_revision": revision,
                "enable": True,
            },
        )
        assert result["values"]["autoscale"] == "DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS"
        _assert_values_equal(result["semantic_values"]["levels.red"]["values"], [0.05, 0.4, 0.95])

        params = await _module_params(client, "rgblevels")
        values = params["semantic_values"]
        _assert_values_equal(values["levels.red"]["values"], [0.05, 0.4, 0.95])
        _assert_values_equal(values["levels.green"]["values"], [0.0, 0.5, 1.0])
        _assert_values_equal(values["levels.blue"]["values"], [0.0, 0.5, 1.0])
        assert values["levels.linked"]["active"] is False
        assert values["levels.red"]["active"] is True

        after_render = await _render(client)
        assert after_render != before_render

        ordering_before = await _module_params(client, "rgblevels")
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "rgblevels",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"levels.red": _vec([0.5, 0.5, 0.9])},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        assert await _module_params(client, "rgblevels") == ordering_before


# ---------------------------------------------------------------------------
# 4. borders: two independent, always-writable colors
# ---------------------------------------------------------------------------


async def test_borders_frame_color_and_color_writes(darktable_session):
    """'frame_size' defaults to 0.0 (invisible frame line); one request
    enables the module, sets a nonzero frame_size, and writes
    'frame_color' -- the fixture requirement for the color to be visually
    effective -- with a render-diff; a second gate writes the border fill
    'color' itself (already visible: 'size' defaults to 0.1)."""
    async with harness.connected_client(darktable_session) as client:
        before_render = await _render(client)

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "borders",
                "instance": 0,
                "values": {"frame_size": 0.1},
                "semantic_values": {"frame_color": _vec([1.0, 0.0, 0.0])},
                "expected_revision": revision,
                "enable": True,
            },
        )
        _assert_values_equal(result["semantic_values"]["frame_color"]["values"], [1.0, 0.0, 0.0])
        params = await _module_params(client, "borders")
        _assert_values_equal(params["semantic_values"]["frame_color"]["values"], [1.0, 0.0, 0.0])

        after_frame_render = await _render(client)
        assert after_frame_render != before_render

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "borders",
                "instance": 0,
                "values": {},
                "semantic_values": {"color": _vec([0.0, 0.0, 1.0])},
                "expected_revision": revision,
            },
        )
        _assert_values_equal(result["semantic_values"]["color"]["values"], [0.0, 0.0, 1.0])
        params = await _module_params(client, "borders")
        _assert_values_equal(params["semantic_values"]["color"]["values"], [0.0, 0.0, 1.0])

        after_color_render = await _render(client)
        assert after_color_render != after_frame_render


# ---------------------------------------------------------------------------
# 5. watermark: needs an XMP-seeded fixture (simple-text.svg, not the
#    default darktable.svg, is the only marker that consumes
#    $(WATERMARK_COLOR) -- watermark.c:1387) on a fresh, dedicated instance
# ---------------------------------------------------------------------------


def _watermark_params_v7(filename: str, text: str = "MCP") -> bytes:
    """dt_iop_watermark_params_t, version 7 (src/iop/watermark.c:88-112):
    float opacity, scale, xoffset, yoffset; int alignment; float rotate;
    int scale_base, scale_img, scale_svg; char filename[256];
    char text[512]; float color[3]; char font[64]."""
    return struct.pack(
        "<4f i f 3i 256s 512s 3f 64s",
        100.0, 100.0, 0.0, 0.0, 4, 0.0, 0, 0, 0,
        filename.encode(), text.encode(), 0.0, 0.0, 0.0,
        b"DejaVu Sans 10")


def test_watermark_params_v7_struct_size():
    """Pins the C struct layout independent of any live darktable:
    sizeof(dt_iop_watermark_params_t) = 4 floats + 1 int + 1 float +
    3 ints + filename[256] + text[512] + color[3] floats + font[64]."""
    assert struct.calcsize("<4f i f 3i 256s 512s 3f 64s") == 880
    assert 880 == 4 * 4 + 4 + 4 + 3 * 4 + 256 + 512 + 12 + 64
    assert len(_watermark_params_v7("simple-text.svg")) == 880


# Captured from a live darktable run: launched with
# write_sidecar_files=on_change, watermark enabled via set_module_enabled
# (a plain scalar/enable mutation, unaffected by the vector-write blocker
# documented at the top of this module), then the resulting
# <workdir>/test.ARW.xmp copied verbatim. The only edits below are the
# ones the task brief calls for: darktable:params was swapped for
# _watermark_params_v7("simple-text.svg").hex() and darktable:modversion
# pinned to "7" (the live capture already used the current watermark op
# version, so this is a no-op beyond making the pin explicit);
# darktable:enabled is "0" so the test's own set_module_params(enable=True)
# below is what turns the module on, mirroring every other gate's pattern
# of enabling through the call under test rather than the fixture.
_WATERMARK_XMP_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="darktable MCP test fixture">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:darktable="http://darktable.sf.net/"
   darktable:xmp_version="5"
   darktable:raw_params="0"
   darktable:auto_presets_applied="1"
   darktable:history_end="1">
   <darktable:history>
    <rdf:Seq>
     <rdf:li
      darktable:num="0"
      darktable:operation="watermark"
      darktable:enabled="0"
      darktable:modversion="7"
      darktable:params="{params_hex}"
      darktable:multi_name=""
      darktable:multi_priority="0"
      darktable:blendop_version="11"
      darktable:blendop_params="gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg="/>
    </rdf:Seq>
   </darktable:history>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
""".format(params_hex=_watermark_params_v7("simple-text.svg").hex())


async def test_watermark_color_write_with_seeded_simple_text_svg(
    darktable_binary, display_server, tmp_path_factory
):
    """Needs its own dedicated instance (not the shared ``darktable_session``)
    because it must launch with a pre-seeded .xmp: 'filename' has no
    remotely-writable wire representation (watermark.c's string fields are
    read-only over the protocol), so selecting simple-text.svg can only
    happen through history seeded at import time. Once seeded, the module
    is enabled and 'color' written through the normal wire path, and the
    render-diff proves the write was visually effective -- which the
    default darktable.svg marker could never prove, since it does not
    reference $(WATERMARK_COLOR) at all (watermark.c:1387)."""
    workdir = tmp_path_factory.mktemp("dt-watermark")
    bus = harness.PrivateSessionBus().start()
    instance = harness.DarktableInstance(
        binary=darktable_binary,
        workdir=workdir,
        display=display_server.display,
        dbus_address=bus.address,
    )
    sidecar_name = f"test{harness.test_image().suffix}.xmp"
    try:
        instance.launch(sidecar_files={sidecar_name: _WATERMARK_XMP_TEMPLATE.encode("utf-8")})
        instance.wait_for_discovery()

        async with harness.connected_client(instance) as client:
            before = await _module_params(client, "watermark")
            assert before["enabled"] is False

            before_render = await _render(client)

            revision = await harness.wait_for_stable_revision(client)
            result = await client.call(
                "set_module_params",
                {
                    "module": "watermark",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"color": _vec([1.0, 0.0, 0.0])},
                    "expected_revision": revision,
                    "enable": True,
                },
            )
            _assert_values_equal(result["semantic_values"]["color"]["values"], [1.0, 0.0, 0.0])

            params = await _module_params(client, "watermark")
            _assert_values_equal(params["semantic_values"]["color"]["values"], [1.0, 0.0, 0.0])

            after_render = await _render(client)
            assert after_render != before_render
    finally:
        instance.close()
        bus.stop()


# ---------------------------------------------------------------------------
# 6. undo (mirrors test_curves.py:178)
# ---------------------------------------------------------------------------


async def test_vector_write_undo_restores_prior_values(darktable_session):
    """One undo of a vector write restores the exact prior component
    values. Uses channelmixerrgb's independent 'grey' row (untouched by
    gate 2's 'red' guard test and its normalize_R flip) so this gate is
    self-contained within the shared session."""
    async with harness.connected_client(darktable_session) as client:
        # Separator edit: keep the vector patch below out of any coalesced
        # undo group with an earlier channelmixerrgb edit -- same
        # discipline as test_curves.py:178's own exposure separator.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.1}},
        )
        await harness.wait_for_stable_revision(client)

        prior = await _module_params(client, "channelmixerrgb")
        prior_grey = prior["semantic_values"]["grey"]["values"]

        edited = await client.call(
            "set_module_params",
            {
                "module": "channelmixerrgb",
                "instance": 0,
                "values": {},
                "semantic_values": {"grey": _vec([0.3, 0.3, 0.3])},
            },
        )
        _assert_values_equal(edited["semantic_values"]["grey"]["values"], [0.3, 0.3, 0.3])

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await _module_params(client, "channelmixerrgb")
        _assert_values_equal(reverted["semantic_values"]["grey"]["values"], prior_grey)


# ---------------------------------------------------------------------------
# 7. stale revision (mirrors test_curves.py:307)
# ---------------------------------------------------------------------------


async def test_vector_write_stale_revision_changes_nothing(darktable_session):
    """A vector patch carrying a stale expected_revision is rejected with
    a retryable revision_conflict and leaves state untouched. Uses
    channelmixerrgb's independent 'lightness' row, free of the other
    gates' 'red'/'grey' rows in this shared session."""
    async with harness.connected_client(darktable_session) as client:
        current = await harness.wait_for_stable_revision(client)
        before = await _module_params(client, "channelmixerrgb")

        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "channelmixerrgb",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"lightness": _vec([0.5, 0.5, 0.5])},
                    "expected_revision": current + 1000,  # deliberately wrong
                },
            )
        assert excinfo.value.code == "revision_conflict"
        assert excinfo.value.retryable is True

        after = await client.call("get_state")
        assert after["revision"] == current
        assert await _module_params(client, "channelmixerrgb") == before
