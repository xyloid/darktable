"""Tier-3 live gates for drawn-mask creation, geometry, and ownership.

``show_mask`` is the primary oracle throughout: it produces a grayscale JPEG
whose light pixels are the effective blend mask.  All spatial assertions use
the same 512px preview so threshold and centroid calculations stay comparable.
The shared GUI session makes the gates deliberately definition-ordered; names
on the persistent circle let later gates find precisely the form created by
the first gate, while the distortion and undo probes clean up after themselves.
"""

from __future__ import annotations

import base64
import io

import pytest
from PIL import Image, ImageStat

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

MASK_MAX_PX = 512
MASK_QUALITY = 90
CIRCLE_NAME = "tier3 moving circle"
DISTORTION_NAME = "tier3 distortion circle"
UNDO_NAME = "tier3 undo circle"


async def _module_params(client, module: str, instance: int = 0) -> dict:
    return await client.call("get_module_params", {"module": module, "instance": instance})


async def _render(client, *, show_mask: bool = False) -> Image.Image | bytes:
    """Render a pinned preview, retrying the documented transient once."""
    await harness.wait_for_stable_revision(client)
    params: dict = {"max_px": MASK_MAX_PX, "quality": MASK_QUALITY}
    if show_mask:
        params["show_mask"] = {"op": "exposure", "instance": 0}
    for attempt in (1, 2):
        try:
            result = await client.call("render_preview", params)
            data = base64.b64decode(result["data"])
            if not show_mask:
                return data
            with Image.open(io.BytesIO(data)) as encoded:
                mask = encoded.convert("L").copy()
            assert mask.size[0] == MASK_MAX_PX
            return mask
        except Exception as exc:
            # The existing live suites accept exactly one preview_failed retry;
            # retain that behavior without treating other failures as flaky.
            if getattr(exc, "code", None) != "preview_failed" or attempt == 2:
                raise
    raise AssertionError("unreachable")


def _box(image: Image.Image, cx: float, cy: float, radius: float) -> tuple[int, int, int, int]:
    width, height = image.size
    return (
        max(0, int(round((cx - radius) * width))),
        max(0, int(round((cy - radius) * height))),
        min(width, int(round((cx + radius) * width))),
        min(height, int(round((cy + radius) * height))),
    )


def _mean_at(image: Image.Image, cx: float, cy: float, radius: float = 0.05) -> float:
    return ImageStat.Stat(image.crop(_box(image, cx, cy, radius))).mean[0]


def _inside_outside_ratio(image: Image.Image, center: tuple[float, float]) -> tuple[float, float, float]:
    inside = _mean_at(image, *center)
    # The upper-left corner is well outside both requested circle positions.
    outside = _mean_at(image, 0.04, 0.04)
    return inside, outside, inside / max(outside, 1.0)


def _bright_centroid(image: Image.Image) -> tuple[float, float]:
    """Intensity-weighted centroid of display-mask pixels above mid-gray."""
    width, height = image.size
    total = x_total = y_total = 0.0
    for index, value in enumerate(image.tobytes()):
        # ``tobytes`` is warning-free and flat for this single-channel image.
        x = index % width
        py = index // width
        weight = max(float(value) - 128.0, 0.0)
        total += weight
        x_total += x * weight
        y_total += py * weight
    assert total > 0.0, "drawn mask contains no bright mass"
    return x_total / total / width, y_total / total / height


async def _list_shapes(client) -> list[dict]:
    await harness.wait_for_stable_revision(client)
    return (await _coordinate_call(client, "list_mask_shapes"))["shapes"]


async def _coordinate_call(client, method: str, params: dict | None = None) -> dict:
    """Retry the coordinate APIs' bounded pipe-freshness timeout exactly once.

    These calls perform their own bounded wait for the live preview pipe.  A
    retry is valid only for the explicit retryable ``retry_later`` response;
    every other protocol error, including a second timeout, remains visible.
    """
    for attempt in (1, 2):
        try:
            return await client.call(method, params)
        except ProtocolError as exc:
            if exc.code != "retry_later" or exc.retryable is not True or attempt == 2:
                raise
    raise AssertionError("unreachable")


async def _shape_named(client, name: str) -> dict:
    for shape in await _list_shapes(client):
        if shape["name"] == name:
            return shape
    raise AssertionError(f"required shape {name!r} was not found")


# ---------------------------------------------------------------------------
# 1. hello capability
# ---------------------------------------------------------------------------


async def test_hello_advertises_mask_shapes(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info
        assert info is not None
        assert "mask_shapes" in info["capabilities"]


# ---------------------------------------------------------------------------
# 2. attached circle confines the blend effect
# ---------------------------------------------------------------------------


async def test_create_circle_confines_effect(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        # Earlier blend gates leave exposure in varying states.  Force a clean
        # off state so attaching this first drawn form must report `drawn`,
        # rather than retaining a parametric bit from an earlier gate.
        await client.call(
            "set_module_params",
            {
                "module": "exposure",
                "instance": 0,
                "values": {"exposure": 2.0},
                "blend": {"mask_mode": "off"},
                "enable": True,
            },
        )
        # The export/cache warm-up gives the GUI preview worker a chance to
        # establish a valid live pipe before the ordinary placement gates.
        # Gate 4 deliberately skips it to exercise mutate-then-create freshness.
        await _render(client)
        revision = await harness.wait_for_stable_revision(client)
        created = await _coordinate_call(
            client,
            "create_mask_shape",
            {
                "type": "circle",
                "space": "preview",
                "name": CIRCLE_NAME,
                "geometry": {
                    "center": [0.5, 0.5],
                    "radius": 0.15,
                    "border": 0.02,
                },
                "attach": {"op": "exposure", "instance": 0},
                "expected_revision": revision,
            },
        )
        shape = created["shape"]
        assert any(use["op"] == "exposure" and use["instance"] == 0 for use in shape["used_by"])

        mask = await _render(client, show_mask=True)
        assert isinstance(mask, Image.Image)
        inside, outside, ratio = _inside_outside_ratio(mask, (0.5, 0.5))
        assert inside > 100.0
        assert ratio > 3.0, (inside, outside, ratio)

        params = await _module_params(client, "exposure")
        assert params["blend"]["mask_mode"] == "drawn"


# ---------------------------------------------------------------------------
# 3. geometry update moves the visible bright mass
# ---------------------------------------------------------------------------


async def test_update_moves_region(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        shape = await _shape_named(client, CIRCLE_NAME)
        revision = await harness.wait_for_stable_revision(client)
        await _coordinate_call(
            client,
            "update_mask_shape",
            {
                "id": shape["id"],
                "space": "preview",
                "geometry": {
                    "center": [0.25, 0.25],
                    "radius": 0.15,
                    "border": 0.02,
                },
                "expected_revision": revision,
            },
        )
        mask = await _render(client, show_mask=True)
        assert isinstance(mask, Image.Image)
        centroid = _bright_centroid(mask)
        assert centroid[0] == pytest.approx(0.25, abs=0.04)
        assert centroid[1] == pytest.approx(0.25, abs=0.04)


# ---------------------------------------------------------------------------
# 4. transformed preview space round-trips through raw storage
# ---------------------------------------------------------------------------


async def test_distortion_roundtrip_and_freshness(darktable_session):
    """A rotation dirties the pipe; the immediate create must wait for its
    transformed preview coordinates before storing raw geometry.

    The brief calls this a crop rotation, but this source tree's crop module
    has no rotation parameter; ``rotatepixels.angle`` is the real rotational
    distortion operation and is restored before leaving this gate.
    """
    point = (0.68, 0.31)
    async with harness.connected_client(darktable_session) as client:
        prior = await _module_params(client, "rotatepixels")
        try:
            # Deliberately do not wait between the distortion mutation and
            # first create attempt: if its bounded freshness wait times out,
            # _coordinate_call performs the documented single retry.
            await client.call(
                "set_module_params",
                {
                    "module": "rotatepixels",
                    "instance": 0,
                    "values": {"angle": 15.0},
                    "enable": True,
                },
            )
            created = await _coordinate_call(
                client,
                "create_mask_shape",
                {
                    "type": "circle",
                    "space": "preview",
                    "name": DISTORTION_NAME,
                    "geometry": {
                        "center": list(point),
                        "radius": 0.08,
                        "border": 0.01,
                    },
                    "attach": None,
                },
            )
            shape = created["shape"]
            listed = await _shape_named(client, DISTORTION_NAME)
            center = listed["geometry"]["center"]
            assert center[0] == pytest.approx(point[0], abs=0.04)
            assert center[1] == pytest.approx(point[1], abs=0.04)

            await client.call("delete_mask_shape", {"id": shape["id"]})
        finally:
            await client.call(
                "set_module_params",
                {
                    "module": "rotatepixels",
                    "instance": 0,
                    "values": {"angle": prior["values"]["angle"]},
                    "enable": prior["enabled"],
                },
            )


# ---------------------------------------------------------------------------
# 5. detach and delete
# ---------------------------------------------------------------------------


async def test_detach_makes_effect_uniform_then_delete_noops(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        shape = await _shape_named(client, CIRCLE_NAME)
        before_detach = await _render(client)
        assert isinstance(before_detach, bytes)
        revision = await harness.wait_for_stable_revision(client)
        await _coordinate_call(
            client,
            "set_mask_attachment",
            {
                "op": "exposure",
                "instance": 0,
                "shape_id": shape["id"],
                "attached": False,
                "expected_revision": revision,
            },
        )
        after_detach = await _render(client)
        assert isinstance(after_detach, bytes)
        assert after_detach != before_detach

        mask = await _render(client, show_mask=True)
        assert isinstance(mask, Image.Image)
        inside, outside, ratio = _inside_outside_ratio(mask, (0.25, 0.25))
        assert ratio == pytest.approx(1.0, abs=0.12), (inside, outside, ratio)

        revision = await harness.wait_for_stable_revision(client)
        await client.call("delete_mask_shape", {"id": shape["id"], "expected_revision": revision})
        after_delete = await _render(client)
        assert isinstance(after_delete, bytes)
        assert after_delete == after_detach
        assert shape["id"] not in {item["id"] for item in await _list_shapes(client)}


# ---------------------------------------------------------------------------
# 6. existing deferred brush forms remain attachable but not editable
# ---------------------------------------------------------------------------


async def test_deferred_type_listed_non_editable(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before_shapes = await _list_shapes(client)
        brushes = [shape for shape in before_shapes if shape["type"] == "brush"]
        if not brushes:
            pytest.skip("no fixture edit containing a brush form is available to the live session")
        brush = brushes[0]
        assert brush["editable"] is False
        assert "geometry" not in brush
        before_mode = (await _module_params(client, "exposure"))["blend"]["mask_mode"]
        attachment_recorded = False
        try:
            revision = await harness.wait_for_stable_revision(client)
            result = await _coordinate_call(
                client,
                "set_mask_attachment",
                {
                    "op": "exposure",
                    "instance": 0,
                    "shape_id": brush["id"],
                    "attached": True,
                    "expected_revision": revision,
                },
            )
            attachment_recorded = True
            assert result["shape"]["id"] == brush["id"]
            assert any(
                use["op"] == "exposure" and use["instance"] == 0
                for use in result["shape"]["used_by"]
            )
        finally:
            if attachment_recorded:
                # ``used_by`` is transitive, so it cannot reveal whether an
                # exposure edge existed directly before the call.  Undoing
                # the call's guaranteed history item is the exact restoration
                # primitive for either direct, nested, or absent membership.
                await harness.undo_latest(client)
                assert await _list_shapes(client) == before_shapes
                restored = await _module_params(client, "exposure")
                assert restored["blend"]["mask_mode"] == before_mode


# ---------------------------------------------------------------------------
# 7. two independent history items undo create then attachment
# ---------------------------------------------------------------------------


async def test_undo_twice_restores_create_attach(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before_shapes = await _list_shapes(client)
        before_mode = (await _module_params(client, "exposure"))["blend"]["mask_mode"]
        before_history = await client.call("get_history", {"limit": 100})

        revision = await harness.wait_for_stable_revision(client)
        created = await _coordinate_call(
            client,
            "create_mask_shape",
            {
                "type": "circle",
                "space": "preview",
                "name": UNDO_NAME,
                "geometry": {
                    "center": [0.58, 0.52],
                    "radius": 0.10,
                    "border": 0.02,
                },
                "attach": {"op": "exposure", "instance": 0},
                "expected_revision": revision,
            },
        )
        assert any(
            use["op"] == "exposure" and use["instance"] == 0
            for use in created["shape"]["used_by"]
        )
        after_history = await client.call("get_history", {"limit": 100})
        assert len(after_history["items"]) == len(before_history["items"]) + 2

        # These are two real undos; coordinate calls above retry only the
        # documented bounded freshness timeout.
        await harness.undo_latest(client)
        await harness.undo_latest(client)
        assert await _list_shapes(client) == before_shapes
        assert (await _module_params(client, "exposure"))["blend"]["mask_mode"] == before_mode
