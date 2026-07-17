"""Milestone-5 live gates: the sampled-response (bands) class and its four
module adapters (atrous, denoiseprofile, rawdenoise, lowlight), patterned
on ``test_vectors_milestone4.py`` -- session fixtures, the render-diff
helper, history-length assertions, undo, and stale-revision gates all
mirror that file's shapes, adapted to band wire entries
(``{"class": "bands", "y": [...], "x"?: [...]}`` under ``semantic_values``,
speaking the raw wire protocol directly -- the ``bands=`` sugar on the
``set_module_params`` MCP tool is a FastMCP-layer convenience this suite
does not go through, exactly as the curve and vector gates never go
through their own tool-layer sugar).

Error-detail note: the shipped band engine reports ``invalid_value``
details as ``{"parameter", "array", "index", "constraint"}`` -- the vector
engine's convention extended with an ``"array"`` member, since a band-set
has two arrays a violation can be attributed to -- not the design draft's
``band_index`` spelling. The unit suite pins this shape
(``test_remote_band.c: assert_band_error_details``) and gate 10 below
asserts the same.

Ordering note: these tests share the session-scoped darktable instance and
run in definition order. The atrous gates build on one another (gate 8's
x-shift is the stored state gate 9's twin-conflict rejection must leave
untouched); every gate captures the state it depends on up front, and
same-module history coalescing is broken where it matters by interposing
an exposure edit (the ``test_curves.py`` discipline).
"""

from __future__ import annotations

import base64

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

ATROUS_BAND_IDS = (
    "bands.luma",
    "bands.chroma",
    "bands.sharpness",
    "bands.luma_threshold",
    "bands.chroma_threshold",
)
DENOISEPROFILE_BAND_IDS = (
    "bands.all",
    "bands.red",
    "bands.green",
    "bands.blue",
    "bands.y0",
    "bands.u0v0",
)


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


def _band(y: list[float], x: list[float] | None = None) -> dict:
    entry: dict = {"class": "bands", "y": y}
    if x is not None:
        entry["x"] = x
    return entry


def _assert_samples_equal(actual: list[float], expected: list[float], *, abs_tol: float = 1e-6) -> None:
    # Stored samples are 32-bit floats; compare with a tolerance well above
    # float32 rounding but far below any real difference -- same convention
    # as the curve/vector gates.
    assert len(actual) == len(expected)
    for a, e in zip(actual, expected):
        assert a == pytest.approx(e, abs=abs_tol)


# ---------------------------------------------------------------------------
# 1. hello capability
# ---------------------------------------------------------------------------


async def test_hello_advertises_band_params(darktable_session):
    """The live hello advertises `band_params` alongside the earlier
    semantic-class capabilities."""
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info  # populated by the hello handshake
        assert info is not None
        assert "band_params" in info["capabilities"]
        assert "curve_params" in info["capabilities"]
        assert "vector_params" in info["capabilities"]


# ---------------------------------------------------------------------------
# 2. atrous schema: five interior-x bands, twin links, octaves denylist
# ---------------------------------------------------------------------------


async def test_atrous_schema_bands_twins_and_octaves_denylist(darktable_session):
    """The live atrous schema lists the five band fields in registry order
    with interior x policy, the 0.001 minimum gap, and the two twin-group
    links; the native `x`/`y` storage arrays are read-only with
    `represented_by` back-references; `octaves` is denylisted."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call("get_module_schema", {"module": "atrous"})

        semantic = {f["name"]: f for f in schema["semantic_fields"]}
        assert tuple(f["name"] for f in schema["semantic_fields"]) == ATROUS_BAND_IDS
        for name in ATROUS_BAND_IDS:
            field = semantic[name]
            assert field["class"] == "bands"
            assert field["count"] == 6
            assert field["x_policy"] == "interior"
            assert field["min_gap"] == pytest.approx(0.001)
            assert field["y_range"]["minimum"] == pytest.approx(0.0)
            assert field["y_range"]["maximum"] == pytest.approx(1.0)
        assert semantic["bands.luma"]["x_shared_with"] == "bands.luma_threshold"
        assert semantic["bands.luma_threshold"]["x_shared_with"] == "bands.luma"
        assert semantic["bands.chroma"]["x_shared_with"] == "bands.chroma_threshold"
        assert semantic["bands.chroma_threshold"]["x_shared_with"] == "bands.chroma"
        assert "x_shared_with" not in semantic["bands.sharpness"]

        native = {f["name"]: f for f in schema["fields"]}
        for field_name in ("x", "y"):
            assert native[field_name]["writable"] is False
            assert set(native[field_name]["represented_by"]) == set(ATROUS_BAND_IDS)
        assert native["octaves"]["writable"] is False


# ---------------------------------------------------------------------------
# 3. denoiseprofile schema: six fixed-x channels of seven bands
# ---------------------------------------------------------------------------


async def test_denoiseprofile_schema_six_fixed_channels(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call("get_module_schema", {"module": "denoiseprofile"})

        semantic = {f["name"]: f for f in schema["semantic_fields"]}
        assert tuple(f["name"] for f in schema["semantic_fields"]) == DENOISEPROFILE_BAND_IDS
        for name in DENOISEPROFILE_BAND_IDS:
            field = semantic[name]
            assert field["class"] == "bands"
            assert field["count"] == 7
            assert field["x_policy"] == "fixed"
            # Fixed-policy fields carry neither the interior-only gap nor a
            # twin link.
            assert "min_gap" not in field
            assert "x_shared_with" not in field


# ---------------------------------------------------------------------------
# 4. lowlight: y write and interior-x write both round-trip
# ---------------------------------------------------------------------------


async def test_lowlight_y_and_interior_x_writes_roundtrip(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        new_y = [0.3, 0.4, 0.5, 0.6, 0.7, 0.8]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "lowlight",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.transition": _band(new_y)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.transition"]["y"], new_y)
        params = await _module_params(client, "lowlight")
        _assert_samples_equal(params["semantic_values"]["bands.transition"]["y"], new_y)

        # Interior x: endpoints pinned to the stored 0.0/1.0, interior nodes
        # shifted off the defaults.
        new_x = [0.0, 0.1, 0.3, 0.55, 0.8, 1.0]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "lowlight",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.transition": _band(new_y, new_x)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.transition"]["x"], new_x)
        params = await _module_params(client, "lowlight")
        _assert_samples_equal(params["semantic_values"]["bands.transition"]["x"], new_x)
        _assert_samples_equal(params["semantic_values"]["bands.transition"]["y"], new_y)


# ---------------------------------------------------------------------------
# 5. rawdenoise: channel y write leaves sibling scalar untouched
# ---------------------------------------------------------------------------


async def test_rawdenoise_green_y_write_leaves_threshold(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "rawdenoise")
        threshold_before = before["values"]["threshold"]

        new_y = [0.2, 0.4, 0.6, 0.8, 1.0]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "rawdenoise",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.green": _band(new_y)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.green"]["y"], new_y)

        params = await _module_params(client, "rawdenoise")
        _assert_samples_equal(params["semantic_values"]["bands.green"]["y"], new_y)
        assert params["values"]["threshold"] == pytest.approx(threshold_before)


# ---------------------------------------------------------------------------
# 6. denoiseprofile: u0v0 y write round-trips
# ---------------------------------------------------------------------------


async def test_denoiseprofile_u0v0_y_write_roundtrips(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        new_y = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "denoiseprofile",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.u0v0": _band(new_y)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.u0v0"]["y"], new_y)
        params = await _module_params(client, "denoiseprofile")
        _assert_samples_equal(params["semantic_values"]["bands.u0v0"]["y"], new_y)


# ---------------------------------------------------------------------------
# 7. atrous: y is NOT shared between twins, only x
# ---------------------------------------------------------------------------


async def test_atrous_luma_y_write_leaves_threshold_y(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "atrous")
        threshold_y_before = before["semantic_values"]["bands.luma_threshold"]["y"]

        new_y = [0.6, 0.65, 0.7, 0.6, 0.55, 0.5]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "atrous",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.luma": _band(new_y)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.luma"]["y"], new_y)

        params = await _module_params(client, "atrous")
        _assert_samples_equal(params["semantic_values"]["bands.luma"]["y"], new_y)
        _assert_samples_equal(
            params["semantic_values"]["bands.luma_threshold"]["y"], threshold_y_before
        )


# ---------------------------------------------------------------------------
# 8. atrous: an x write mirrors to the twin's native x
# ---------------------------------------------------------------------------


async def test_atrous_x_write_mirrors_to_twin(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "atrous")
        luma_y = before["semantic_values"]["bands.luma"]["y"]
        chroma_x_before = before["semantic_values"]["bands.chroma"]["x"]

        new_x = [0.0, 0.15, 0.4, 0.6, 0.85, 1.0]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "atrous",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.luma": _band(luma_y, new_x)},
                "expected_revision": revision,
            },
        )
        _assert_samples_equal(result["semantic_values"]["bands.luma"]["x"], new_x)

        params = await _module_params(client, "atrous")
        values = params["semantic_values"]
        _assert_samples_equal(values["bands.luma"]["x"], new_x)
        # Live twin mirroring: the threshold channel shares the same stored x.
        _assert_samples_equal(values["bands.luma_threshold"]["x"], new_x)
        # The non-twin channels' x is untouched.
        _assert_samples_equal(values["bands.chroma"]["x"], chroma_x_before)


# ---------------------------------------------------------------------------
# 9. atrous: twin-conflict request rejected atomically
# ---------------------------------------------------------------------------


async def test_atrous_twin_conflict_rejected(darktable_session):
    """Two entries in one request carrying different x for the same twin
    group are rejected with invalid_value, changing nothing."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "atrous")
        luma_y = before["semantic_values"]["bands.luma"]["y"]
        threshold_y = before["semantic_values"]["bands.luma_threshold"]["y"]

        x_a = [0.0, 0.15, 0.4, 0.6, 0.85, 1.0]
        x_b = [0.0, 0.16, 0.4, 0.6, 0.85, 1.0]  # differs in one component
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "atrous",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "bands.luma": _band(luma_y, x_a),
                        "bands.luma_threshold": _band(threshold_y, x_b),
                    },
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        assert await _module_params(client, "atrous") == before


# ---------------------------------------------------------------------------
# 10. validation error shapes: y domain, x on a fixed-policy module
# ---------------------------------------------------------------------------


async def test_band_y_out_of_range_and_fixed_x_rejections(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        lowlight_before = await _module_params(client, "lowlight")
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "lowlight",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"bands.transition": _band([1.5] + [0.5] * 5)},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        details = excinfo.value.details
        assert details["parameter"] == "bands.transition"
        assert details["array"] == "y"
        assert details["index"] == 0
        assert details["constraint"] == "domain"
        assert await _module_params(client, "lowlight") == lowlight_before

        rawdenoise_before = await _module_params(client, "rawdenoise")
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "rawdenoise",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "bands.all": _band([0.5] * 5, [0.0, 0.25, 0.5, 0.75, 1.0])
                    },
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert await _module_params(client, "rawdenoise") == rawdenoise_before


# ---------------------------------------------------------------------------
# 11. mixed scalar + band request is one history item
# ---------------------------------------------------------------------------


async def test_scalar_and_band_is_one_history_item(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        # Break same-module coalescing with the earlier atrous gates, so
        # "exactly one new item" is attributable to this patch.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.2}, "enable": True},
        )
        await harness.wait_for_stable_revision(client)
        history_before = await client.call("get_history", {"limit": 50})

        await client.call(
            "set_module_params",
            {
                "module": "atrous",
                "instance": 0,
                "values": {"mix": 0.8},
                "semantic_values": {"bands.luma": _band([0.55] * 6)},
            },
        )
        await harness.wait_for_stable_revision(client)
        history_after = await client.call("get_history", {"limit": 50})

        assert len(history_after["items"]) == len(history_before["items"]) + 1
        assert history_after["items"][-1]["op"] == "atrous"


# ---------------------------------------------------------------------------
# 12. undo (mirrors the vector undo gate)
# ---------------------------------------------------------------------------


async def test_band_write_undo_restores_prior_values(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        # Separator edit: keep the band patch below out of any coalesced
        # undo group with an earlier lowlight edit.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.3}},
        )
        await harness.wait_for_stable_revision(client)

        prior = await _module_params(client, "lowlight")
        prior_y = prior["semantic_values"]["bands.transition"]["y"]
        prior_x = prior["semantic_values"]["bands.transition"]["x"]

        edited = await client.call(
            "set_module_params",
            {
                "module": "lowlight",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.transition": _band([0.25] * 6)},
            },
        )
        _assert_samples_equal(edited["semantic_values"]["bands.transition"]["y"], [0.25] * 6)

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await _module_params(client, "lowlight")
        _assert_samples_equal(reverted["semantic_values"]["bands.transition"]["y"], prior_y)
        _assert_samples_equal(reverted["semantic_values"]["bands.transition"]["x"], prior_x)


# ---------------------------------------------------------------------------
# 13. stale revision (mirrors the vector stale gate)
# ---------------------------------------------------------------------------


async def test_band_write_stale_revision_changes_nothing(darktable_session):
    """A band patch carrying a stale expected_revision is rejected with a
    retryable revision_conflict and leaves state untouched. Uses
    denoiseprofile's `bands.y0`, a row no other gate writes in this shared
    session."""
    async with harness.connected_client(darktable_session) as client:
        current = await harness.wait_for_stable_revision(client)
        before = await _module_params(client, "denoiseprofile")

        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "denoiseprofile",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"bands.y0": _band([0.4] * 7)},
                    "expected_revision": current + 1000,  # deliberately wrong
                },
            )
        assert excinfo.value.code == "revision_conflict"
        assert excinfo.value.retryable is True

        after = await client.call("get_state")
        assert after["revision"] == current
        assert await _module_params(client, "denoiseprofile") == before


# ---------------------------------------------------------------------------
# 14. effectiveness: a strong luma lift visibly changes the render
# ---------------------------------------------------------------------------


async def test_atrous_band_lift_is_visually_effective(darktable_session):
    """Enabling atrous with a maximum bands.luma lift (y = 1.0 across all
    six bands vs the 0.5 default) produces a pixel-different preview --
    the write is not just stored but flows through the pixelpipe."""
    async with harness.connected_client(darktable_session) as client:
        before_render = await _render(client)

        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "atrous",
                "instance": 0,
                "values": {},
                "semantic_values": {"bands.luma": _band([1.0] * 6)},
                "expected_revision": revision,
                "enable": True,
            },
        )
        assert result["enabled"] is True
        _assert_samples_equal(result["semantic_values"]["bands.luma"]["y"], [1.0] * 6)

        after_render = await _render(client)
        assert after_render != before_render
