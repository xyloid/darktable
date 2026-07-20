"""Tier-1 (M-A) blend-settings live gates: the `blend_params` capability,
the live blend schema/read members, and the blend patch path through
`set_module_params` -- patterned on ``test_quantity_milestone6.py``
(session fixtures, the render-diff helper, history-length assertions,
undo), speaking the raw wire protocol directly (the ``blend=`` sugar on
the ``set_module_params`` MCP tool is FastMCP-layer convenience this
suite does not go through, like every earlier semantic-class gate).

Render assertions here are byte-inequality only -- no pixel statistics.
The H6 discipline starts spatial-ratio assertions in M-B, where a mask
render exists to reason about; Tier 1 only needs "the blend edit reached
the pixelpipe".

Ordering note: these tests share the session-scoped darktable instance
and run in definition order. Every gate captures the state it depends on
up front. The blend gates' main target is exposure itself, so same-module
history coalescing is broken by interposing a *temperature* edit (the
inverse of the ``test_curves.py`` discipline, whose targets are other
modules and whose separator is exposure)."""

from __future__ import annotations

import base64

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

MASK_MODE_VOCABULARY = {
    "off",
    "uniform",
    "parametric",
    "drawn",
    "drawn+parametric",
    "raster",
}

RGB_COLORSPACES = {"DEVELOP_BLEND_CS_RGB_DISPLAY", "DEVELOP_BLEND_CS_RGB_SCENE"}


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


async def _separator_edit(client) -> None:
    """Breaks same-module history coalescing before another exposure write:
    a small temperature edit appends a non-exposure history item (`red` is
    directly writable under the quantity coexistence exception)."""
    params = await _module_params(client, "temperature")
    await client.call(
        "set_module_params",
        {
            "module": "temperature",
            "instance": 0,
            "values": {"red": params["values"]["red"] * 1.01},
        },
    )
    await harness.wait_for_stable_revision(client)


# ---------------------------------------------------------------------------
# 1. hello capability
# ---------------------------------------------------------------------------


async def test_hello_advertises_blend_params(darktable_session):
    """The live hello advertises `blend_params` alongside the earlier
    capabilities."""
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info  # populated by the hello handshake
        assert info is not None
        assert "blend_params" in info["capabilities"]
        assert "quantity_params" in info["capabilities"]


# ---------------------------------------------------------------------------
# 2. schema + params carry the blend section
# ---------------------------------------------------------------------------


async def test_schema_and_params_carry_blend_section(darktable_session):
    """`get_module_schema` (with the new `instance` argument) attaches the
    live blend schema; `get_module_params` attaches the current blend
    values in the transition-appendix vocabulary."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call(
            "get_module_schema", {"module": "exposure", "instance": 0}
        )
        assert "blend" in schema
        blend_schema = schema["blend"]
        assert blend_schema["mask_mode"]["values"] == ["off", "uniform"]
        assert blend_schema["colorspace"]["default"] in RGB_COLORSPACES
        assert blend_schema["opacity"]["range"] == [0.0, 100.0]

        params = await _module_params(client, "exposure")
        assert "blend" in params
        blend = params["blend"]
        assert blend["mask_mode"] in MASK_MODE_VOCABULARY
        assert isinstance(blend["opacity"], float)


# ---------------------------------------------------------------------------
# 3. opacity write: readback, one history item, render diff
# ---------------------------------------------------------------------------


async def test_opacity_write_changes_render_and_history(darktable_session):
    """A uniform-mask opacity write reads back complete post-commit blend
    state, lands as exactly one history item, and reaches the pixelpipe
    (byte-different preview)."""
    async with harness.connected_client(darktable_session) as client:
        # Visible exposure delta so halving the blend opacity moves pixels.
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {"exposure": 1.0},
                "enable": True,
            },
        )
        await _separator_edit(client)

        before_render = await _render(client)
        history_before = await client.call("get_history", {"limit": 100})

        result = await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "uniform", "opacity": 50.0},
            },
        )
        assert result["blend"]["mask_mode"] == "uniform"
        assert result["blend"]["opacity"] == pytest.approx(50.0)
        # complete readback, not just the touched members
        assert result["blend"]["mode"] in ("DEVELOP_BLEND_NORMAL2", "DEVELOP_BLEND_BOUNDED")
        assert "feathering_guide" in result["blend"]

        await harness.wait_for_stable_revision(client)
        history_after = await client.call("get_history", {"limit": 100})
        assert len(history_after["items"]) == len(history_before["items"]) + 1

        after_render = await _render(client)
        assert after_render != before_render

        params = await _module_params(client, "exposure")
        assert params["blend"]["opacity"] == pytest.approx(50.0)
        assert params["blend"]["mask_mode"] == "uniform"


# ---------------------------------------------------------------------------
# 4. colorspace switch resets mode; same-patch override is deterministic
# ---------------------------------------------------------------------------


async def test_colorspace_switch_resets_mode(darktable_session):
    """Writing a new blending colorspace resets `mode` to the new space's
    default; a mode sent in the same patch applies on top of the reset."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call(
            "get_module_schema", {"module": "colorbalancergb", "instance": 0}
        )
        default_cs = schema["blend"]["colorspace"]["default"]
        assert default_cs in RGB_COLORSPACES
        (other_cs,) = RGB_COLORSPACES - {default_cs}

        # switch alone: mode resets to the new space's default
        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "instance": 0,
                "values": {},
                "blend": {"colorspace": other_cs},
            },
        )
        assert result["blend"]["colorspace"] == other_cs
        assert result["blend"]["mode"] == "DEVELOP_BLEND_NORMAL2"

        # switch back + same-patch mode override: deterministic composition.
        # A stored colorspace equal to the module default reads back as
        # CS_NONE ("not explicitly chosen"); effective_colorspace carries
        # the resolved space.
        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "instance": 0,
                "values": {},
                "blend": {"colorspace": default_cs, "mode": "DEVELOP_BLEND_MULTIPLY"},
            },
        )
        assert result["blend"]["colorspace"] == "DEVELOP_BLEND_CS_NONE"
        assert result["blend"]["effective_colorspace"] == default_cs
        assert result["blend"]["mode"] == "DEVELOP_BLEND_MULTIPLY"

        # leave the module's blend mode as it was for later files
        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "instance": 0,
                "values": {},
                "blend": {"mode": "DEVELOP_BLEND_NORMAL2"},
            },
        )
        assert result["blend"]["mode"] == "DEVELOP_BLEND_NORMAL2"


# ---------------------------------------------------------------------------
# 5. a blend patch never implicitly enables a disabled module
# ---------------------------------------------------------------------------


async def test_blend_patch_does_not_enable_disabled_module(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        params = await _module_params(client, "colorbalancergb")
        if params["enabled"]:
            await client.call(
                "set_module_enabled",
                {"module": "colorbalancergb", "instance": 0, "enabled": False},
            )
            await harness.wait_for_stable_revision(client)

        result = await client.call(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "instance": 0,
                "values": {},
                "blend": {"opacity": 80.0},
            },
        )
        assert result["enabled"] is False
        assert result["blend"]["opacity"] == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# 6. mask_mode gating: compound targets rejected, off/uniform round-trip
# ---------------------------------------------------------------------------


async def test_mask_mode_rejections_and_roundtrip(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "exposure")

        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "exposure",
                    "instance": 0,
                    "values": {},
                    "blend": {"mask_mode": "drawn"},
                },
            )
        assert excinfo.value.code == "invalid_value"
        details = excinfo.value.details
        assert details["parameter"] == "blend.mask_mode"
        assert (await _module_params(client, "exposure"))["blend"] == before["blend"]

        result = await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "uniform"},
            },
        )
        assert result["blend"]["mask_mode"] == "uniform"

        result = await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "off"},
            },
        )
        assert result["blend"]["mask_mode"] == "off"

        params = await _module_params(client, "exposure")
        assert params["blend"]["mask_mode"] == "off"


# ---------------------------------------------------------------------------
# 7. undo covers the blend edit
# ---------------------------------------------------------------------------


async def test_undo_covers_blend_edit(darktable_session):
    """Undo after a blend write restores the pre-write blend state -- the
    blend block rides the same single history item as everything else."""
    async with harness.connected_client(darktable_session) as client:
        await _separator_edit(client)

        prior = await _module_params(client, "exposure")
        prior_blend = prior["blend"]

        edited = await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "uniform", "opacity": 73.0},
            },
        )
        assert edited["blend"]["opacity"] == pytest.approx(73.0)

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await _module_params(client, "exposure")
        assert reverted["blend"] == prior_blend
