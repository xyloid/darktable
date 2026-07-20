"""Tier-2 parametric-mask and read-only mask-render live gates."""

from __future__ import annotations

import base64
import io

import pytest
from PIL import Image, ImageChops, ImageStat

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

MASK_MAX_PX = 512
MASK_QUALITY = 90
HIGHLIGHT_MARKERS = [0.35, 0.50, 1.0, 1.0]

# Normalized boxes (left, top, right, bottom), pinned by visual inspection
# of img/DSC07350.ARW's default 512px render.
DARK_BACKGROUND_ROI = (0.00, 0.00, 0.10, 0.18)
BRIGHT_SUBJECT_ROI = (0.39, 0.34, 0.51, 0.48)


async def _module_params(client, module: str, instance: int = 0) -> dict:
    return await client.call(
        "get_module_params", {"module": module, "instance": instance}
    )


async def _render_mask(client, op: str = "exposure", instance: int = 0):
    await harness.wait_for_stable_revision(client)
    for attempt in (1, 2):
        try:
            result = await client.call(
                "render_preview",
                {
                    "max_px": MASK_MAX_PX,
                    "quality": MASK_QUALITY,
                    "show_mask": {"op": op, "instance": instance},
                },
            )
            with Image.open(io.BytesIO(base64.b64decode(result["data"]))) as encoded:
                mask = encoded.convert("L").copy()
            return result, mask
        except ProtocolError as exc:
            if exc.code != "preview_failed" or attempt == 2:
                raise
    raise AssertionError("unreachable")


def _mean_in_normalized_roi(image: Image.Image, roi: tuple[float, float, float, float]) -> float:
    width, height = image.size
    left, top, right, bottom = roi
    box = (
        int(round(left * width)),
        int(round(top * height)),
        int(round(right * width)),
        int(round(bottom * height)),
    )
    return ImageStat.Stat(image.crop(box)).mean[0]


async def _set_highlight_mask(client, *, enable: bool | None = True) -> dict:
    params = {
        "module": "exposure",
        "instance": 0,
        "values": {},
        "blend": {
            "colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
            "mask_mode": "parametric",
            "combine": "exclusive",
            "parametric": {
                "Jz_in": {"markers": HIGHLIGHT_MARKERS}
            },
        },
    }
    if enable is not None:
        params["enable"] = enable
    return await client.call("set_module_params", params)


async def _separator_edit(client) -> None:
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


async def test_hello_advertises_parametric_and_mask_render(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info
        assert info is not None
        assert "parametric_mask_params" in info["capabilities"]
        assert "mask_render" in info["capabilities"]


async def test_schema_carries_parametric_contract(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"colorspace": "DEVELOP_BLEND_CS_RGB_SCENE"},
            },
        )
        schema = await client.call(
            "get_module_schema", {"module": "exposure", "instance": 0}
        )
        blend = schema["blend"]
        assert blend["mask_mode"]["values"] == ["off", "uniform", "parametric"]
        assert blend["mask_mode"]["writable"] is True
        assert blend["combine"]["values"] == [
            "exclusive",
            "inclusive",
            "exclusive_inverted",
            "inclusive_inverted",
        ]
        assert "Jz_in" in blend["parametric"]["channels"]
        assert blend["parametric"]["channels"]["Jz_in"]["display_hint"]["unit"] == "%"
        assert blend["allow_inverted_combine"]["write_only"] is True


async def test_disabled_target_mask_is_dark_low_bright_high(darktable_session):
    """The export temporarily enables a disabled target in its throwaway pipe."""
    async with harness.connected_client(darktable_session) as client:
        result = await _set_highlight_mask(client, enable=False)
        assert result["enabled"] is False
        response, mask = await _render_mask(client)
        assert response["mask_of"] == {
            "op": "exposure", "instance": 0, "mask_mode": "parametric"
        }
        assert mask.size[0] == MASK_MAX_PX
        dark = _mean_in_normalized_roi(mask, DARK_BACKGROUND_ROI)
        bright = _mean_in_normalized_roi(mask, BRIGHT_SUBJECT_ROI)
        assert bright > 100.0
        assert bright / max(dark, 1.0) > 3.0
        await client.call(
            "set_module_enabled",
            {"module": "exposure", "instance": 0, "enabled": True},
        )


async def test_combine_flip_changes_mask(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await _set_highlight_mask(client)
        _, exclusive = await _render_mask(client)
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"combine": "inclusive"},
            },
        )
        _, inclusive = await _render_mask(client)
        assert ImageStat.Stat(ImageChops.difference(exclusive, inclusive)).mean[0] > 50.0


async def test_mask_mode_off_renders_full_white(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "off"},
            },
        )
        response, mask = await _render_mask(client)
        assert response["mask_of"]["mask_mode"] == "off"
        minimum, maximum = mask.getextrema()
        assert minimum > 250
        assert maximum <= 255


async def test_parametric_null_disables_channel(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await _set_highlight_mask(client)
        before = await _module_params(client, "exposure")
        assert "Jz_in" in before["blend"]["parametric"]
        # With Jz_in enabled the ramp masks the dark background out (near-zero
        # there) and leaves the bright subject fully covered -- a high-variance
        # mask.
        _, mask_before = await _render_mask(client)
        before_dark = _mean_in_normalized_roi(mask_before, DARK_BACKGROUND_ROI)
        before_stddev = ImageStat.Stat(mask_before).stddev[0]
        assert before_dark < 80

        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"parametric": {"Jz_in": None}},
            },
        )
        after = await _module_params(client, "exposure")
        assert "Jz_in" not in after["blend"]["parametric"]
        # Nulling the only enabled channel disables the parametric ramp: the
        # previously-dark background snaps back to full coverage and the mask's
        # spatial variation collapses. Asserted relatively so the check is
        # immune to opacity / white-point that other tests leave on this module
        # in the session-shared darktable instance (the genuine display-mask
        # white point is ~226, and pure-255 white only comes from the separate
        # off/uniform white-fill path, design amendment 4).
        _, mask_after = await _render_mask(client)
        after_dark = _mean_in_normalized_roi(mask_after, DARK_BACKGROUND_ROI)
        after_stddev = ImageStat.Stat(mask_after).stddev[0]
        assert after_dark > before_dark + 100  # dark region restored to coverage
        assert after_stddev < before_stddev * 0.5  # the ramp is gone


async def test_show_mask_rejects_non_blending_module(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "render_preview",
                {
                    "max_px": MASK_MAX_PX,
                    "quality": MASK_QUALITY,
                    "show_mask": {"op": "rawprepare", "instance": 0},
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert excinfo.value.details == {"parameter": "show_mask.op"}


async def test_parametric_write_is_one_history_item_and_undoable(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {},
                "blend": {"mask_mode": "uniform"},
            },
        )
        await _separator_edit(client)
        before = await client.call("get_history", {"limit": 100})
        result = await _set_highlight_mask(client)
        await harness.wait_for_stable_revision(client)
        after = await client.call("get_history", {"limit": 100})
        assert len(after["items"]) == len(before["items"]) + 1

        await client.call("undo", {"expected_revision": result["revision"]})
        restored = await _module_params(client, "exposure")
        assert restored["blend"]["mask_mode"] == "uniform"
