"""Milestone-6 live gates: the derived-quantity class (temperature's
``wb.temperature`` Kelvin/tint pair) and the two ride-along vector adapters
(negadoctor, colorharmonizer), patterned on ``test_bands_milestone5.py`` --
session fixtures, the render-diff helper, history-length assertions, undo,
and stale-revision gates all mirror that file's shapes, adapted to quantity
wire entries (``{"class": "quantity", "values": {component: number}}``
under ``semantic_values``, speaking the raw wire protocol directly -- the
``quantities=`` sugar on the ``set_module_params`` MCP tool is a
FastMCP-layer convenience this suite does not go through, exactly as the
earlier semantic-class gates never go through their own tool-layer sugar).

These gates are the first place the REAL temperature Kelvin/tint
conversion hooks run: every unit layer fakes them (the hooks need the
darkroom GUI's CAM_to_XYZ matrices and fail closed without them), so the
conversion tolerances pinned here -- ``abs(kelvin delta) <= 5.0`` and
``abs(tint delta) <= 0.01`` on a write/readback round trip -- are asserted
against the live module, with the measured deltas printed for the log.

Error-detail note: quantity ``invalid_value`` details are
``{"parameter", "component", "constraint"}`` when attributable to one
component (the ``component`` member replaces the band engine's
``"array"``/``"index"`` pair, since a quantity's components are named, not
positional) and ``{"parameter", "constraint"}`` otherwise -- the
native-conflict rejection reports the *scalar* field name as its
``parameter``.

Ordering note: these tests share the session-scoped darktable instance and
run in definition order. Every gate captures the state it depends on up
front, and same-module history coalescing is broken where it matters by
interposing an exposure edit (the ``test_curves.py`` discipline). The
colorharmonizer gate's sub-steps run rejected-write and ungated-write
checks BEFORE its composed rule switch, so each check runs under the rule
state it claims to test.
"""

from __future__ import annotations

import base64

import pytest

from darktable_mcp.errors import ProtocolError

from . import harness

pytestmark = pytest.mark.integration

TEMPERATURE_COEFFS = ("red", "green", "blue", "various")


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


def _quantity(values: dict[str, float]) -> dict:
    return {"class": "quantity", "values": values}


def _vec(values: list[float]) -> dict:
    return {"class": "vector", "values": values}


def _assert_values_equal(actual: list[float], expected: list[float], *, abs_tol: float = 1e-6) -> None:
    # Stored components are 32-bit floats; compare with a tolerance well
    # above float32 rounding but far below any real difference -- same
    # convention as the curve/vector/band gates.
    assert len(actual) == len(expected)
    for a, e in zip(actual, expected):
        assert a == pytest.approx(e, abs=abs_tol)


# ---------------------------------------------------------------------------
# 1. hello capability
# ---------------------------------------------------------------------------


async def test_hello_advertises_quantity_params(darktable_session):
    """The live hello advertises `quantity_params` alongside the earlier
    semantic-class capabilities."""
    async with harness.connected_client(darktable_session) as client:
        info = client._conn.server_info  # populated by the hello handshake
        assert info is not None
        assert "quantity_params" in info["capabilities"]
        assert "band_params" in info["capabilities"]
        assert "curve_params" in info["capabilities"]
        assert "vector_params" in info["capabilities"]


# ---------------------------------------------------------------------------
# 2. temperature schema: wb.temperature + coefficient coexistence
# ---------------------------------------------------------------------------


async def test_temperature_schema_quantity_and_coefficient_coexistence(darktable_session):
    """The live temperature schema lists `wb.temperature` with both
    components (units, domains) and `derived: true`; the four coefficient
    scalars keep `writable: true` while carrying
    `represented_by: ["wb.temperature"]` (the coexistence exception --
    unlike every other semantic class, whose native storage goes
    read-only); `preset` stays unwritable."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call("get_module_schema", {"module": "temperature"})

        semantic = {f["name"]: f for f in schema["semantic_fields"]}
        assert "wb.temperature" in semantic
        field = semantic["wb.temperature"]
        assert field["class"] == "quantity"
        assert field["derived"] is True
        assert field["writable"] is True

        components = {c["name"]: c for c in field["components"]}
        assert list(components) == ["temperature", "tint"]
        kelvin = components["temperature"]
        assert kelvin["unit"] == "kelvin"
        assert kelvin["minimum"] == pytest.approx(1901.0)
        assert kelvin["maximum"] == pytest.approx(25000.0)
        tint = components["tint"]
        assert "unit" not in tint  # unitless: the member is omitted, not null
        assert tint["minimum"] == pytest.approx(0.135)
        assert tint["maximum"] == pytest.approx(2.326)

        native = {f["name"]: f for f in schema["fields"]}
        for coeff in TEMPERATURE_COEFFS:
            assert native[coeff]["writable"] is True
            assert native[coeff]["represented_by"] == ["wb.temperature"]
        assert native["preset"]["writable"] is False
        assert "represented_by" not in native["preset"]


# ---------------------------------------------------------------------------
# 3. Kelvin/tint write round-trips within the pinned tolerances
# ---------------------------------------------------------------------------


async def test_temperature_kelvin_tint_write_roundtrips_and_renders(darktable_session):
    """Writing {temperature: 5500, tint: 1.0} through the real conversion
    hooks reads back within the pinned tolerances (<= 5.0 K, <= 0.01 tint;
    measured deltas printed), and the change flows through the pixelpipe
    (pixel-different preview)."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "temperature")
        before_render = await _render(client)

        target_kelvin, target_tint = 5500.0, 1.0
        revision = await harness.wait_for_stable_revision(client)
        request = {
            "module": "temperature",
            "instance": 0,
            "values": {},
            "semantic_values": {
                "wb.temperature": _quantity(
                    {"temperature": target_kelvin, "tint": target_tint}
                )
            },
            "expected_revision": revision,
        }
        if not before["enabled"]:
            # temperature is enabled by default on raw files; only force it
            # on if this fixture image somehow has it off.
            request["enable"] = True
        result = await client.call("set_module_params", request)
        assert result["enabled"] is True

        readback = result["semantic_values"]["wb.temperature"]["values"]
        kelvin_delta = readback["temperature"] - target_kelvin
        tint_delta = readback["tint"] - target_tint
        print(
            f"wb.temperature readback deltas: kelvin {kelvin_delta:+.3f} K, "
            f"tint {tint_delta:+.6f}"
        )
        assert abs(kelvin_delta) <= 5.0
        assert abs(tint_delta) <= 0.01

        # The same values come back on a fresh read, not just in the write
        # response.
        params = await _module_params(client, "temperature")
        stored = params["semantic_values"]["wb.temperature"]["values"]
        assert abs(stored["temperature"] - target_kelvin) <= 5.0
        assert abs(stored["tint"] - target_tint) <= 0.01

        after_render = await _render(client)
        assert after_render != before_render


# ---------------------------------------------------------------------------
# 4. pure-multiplier write still works (coexistence, write side)
# ---------------------------------------------------------------------------


async def test_temperature_scalar_red_write_roundtrips_exactly(darktable_session):
    """A scalar write to `red` alone -- no quantity entry in the patch --
    still works and round-trips exactly: the coexistence exception keeps
    the native coefficients directly writable."""
    async with harness.connected_client(darktable_session) as client:
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "temperature",
                "instance": 0,
                "values": {"red": 2.5},  # exactly representable in float32
                "expected_revision": revision,
            },
        )
        assert result["values"]["red"] == 2.5

        params = await _module_params(client, "temperature")
        assert params["values"]["red"] == 2.5


# ---------------------------------------------------------------------------
# 5. native-conflict rejection
# ---------------------------------------------------------------------------


async def test_temperature_scalar_and_quantity_conflict_rejected(darktable_session):
    """One request writing scalar `red` together with a `wb.temperature`
    entry is rejected with invalid_value / native_conflict, changing
    nothing."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "temperature")

        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "temperature",
                    "instance": 0,
                    "values": {"red": 2.0},
                    "semantic_values": {
                        "wb.temperature": _quantity({"temperature": 6000.0, "tint": 1.0})
                    },
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        details = excinfo.value.details
        assert details["parameter"] == "red"
        assert details["constraint"] == "native_conflict"
        assert await _module_params(client, "temperature") == before


# ---------------------------------------------------------------------------
# 6. component-domain rejection
# ---------------------------------------------------------------------------


async def test_temperature_kelvin_out_of_domain_rejected(darktable_session):
    """Kelvin 26000 (above the 25000 maximum) is rejected with
    invalid_value, the offending component named, state unchanged."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "temperature")

        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "temperature",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "wb.temperature": _quantity({"temperature": 26000.0, "tint": 1.0})
                    },
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "invalid_value"
        details = excinfo.value.details
        assert details["parameter"] == "wb.temperature"
        assert details["component"] == "temperature"
        assert details["constraint"] == "domain"
        assert await _module_params(client, "temperature") == before


# ---------------------------------------------------------------------------
# 7. undo restores the prior coefficients
# ---------------------------------------------------------------------------


async def test_temperature_quantity_write_undo_restores_coeffs(darktable_session):
    """Undo after a quantity write restores the four coefficient scalars --
    compared exactly, since the stored coefficients (unlike the derived
    Kelvin/tint pair) survive the round trip bit-for-bit."""
    async with harness.connected_client(darktable_session) as client:
        # Separator edit: keep the quantity patch below out of any coalesced
        # undo group with the earlier temperature edits.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.25}},
        )
        await harness.wait_for_stable_revision(client)

        prior = await _module_params(client, "temperature")
        prior_coeffs = {name: prior["values"][name] for name in TEMPERATURE_COEFFS}

        edited = await client.call(
            "set_module_params",
            {
                "module": "temperature",
                "instance": 0,
                "values": {},
                "semantic_values": {
                    "wb.temperature": _quantity({"temperature": 4500.0, "tint": 0.9})
                },
            },
        )
        edited_params = await _module_params(client, "temperature")
        assert any(
            edited_params["values"][name] != prior_coeffs[name]
            for name in TEMPERATURE_COEFFS
        )

        undo_result = await harness.undo_latest(client)
        assert undo_result["revision"] > edited["revision"]

        reverted = await _module_params(client, "temperature")
        for name in TEMPERATURE_COEFFS:
            assert reverted["values"][name] == prior_coeffs[name]


# ---------------------------------------------------------------------------
# 8. stale revision changes nothing
# ---------------------------------------------------------------------------


async def test_temperature_quantity_write_stale_revision_changes_nothing(darktable_session):
    """A quantity patch carrying a stale expected_revision is rejected with
    a retryable revision_conflict and leaves state untouched."""
    async with harness.connected_client(darktable_session) as client:
        current = await harness.wait_for_stable_revision(client)
        before = await _module_params(client, "temperature")

        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "temperature",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {
                        "wb.temperature": _quantity({"temperature": 7000.0, "tint": 1.1})
                    },
                    "expected_revision": current + 1000,  # deliberately wrong
                },
            )
        assert excinfo.value.code == "revision_conflict"
        assert excinfo.value.retryable is True

        after = await client.call("get_state")
        assert after["revision"] == current
        assert await _module_params(client, "temperature") == before


# ---------------------------------------------------------------------------
# 9. negadoctor ride-along: dmin write round-trips and renders
# ---------------------------------------------------------------------------


async def test_negadoctor_dmin_write_roundtrips_and_renders(darktable_session):
    """The negadoctor ride-along vector adapter is live end-to-end: a
    `dmin` write (with the module enabled in the same request -- negadoctor
    is off by default) round-trips and produces a pixel-different preview
    (film-negative inversion is about as visible as changes get)."""
    async with harness.connected_client(darktable_session) as client:
        before_render = await _render(client)

        new_dmin = [0.9, 0.5, 0.3]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "negadoctor",
                "instance": 0,
                "values": {},
                "semantic_values": {"dmin": _vec(new_dmin)},
                "expected_revision": revision,
                "enable": True,
            },
        )
        assert result["enabled"] is True
        _assert_values_equal(result["semantic_values"]["dmin"]["values"], new_dmin)

        params = await _module_params(client, "negadoctor")
        _assert_values_equal(params["semantic_values"]["dmin"]["values"], new_dmin)

        after_render = await _render(client)
        assert after_render != before_render


# ---------------------------------------------------------------------------
# 10. colorharmonizer ride-along: rule-gated custom_hue
# ---------------------------------------------------------------------------


async def test_colorharmonizer_custom_hue_gating_and_node_saturation(darktable_session):
    """`custom_hue` is writable only under the custom rule: a write under
    the default rule is rejected (unsupported_field); `node_saturation`
    writes under any rule; the same `custom_hue` write composed with
    `values: {"rule": "DT_COLORHARMONIZER_CUSTOM"}` in one request applies
    and round-trips. Ordered so each sub-step runs under the rule state it
    claims to test (the two default-rule checks come first)."""
    async with harness.connected_client(darktable_session) as client:
        before = await _module_params(client, "colorharmonizer")
        assert before["values"]["rule"] == "DT_COLORHARMONIZER_COMPLEMENTARY"

        new_hues = [0.1, 0.35, 0.6, 0.85]

        # (a) Under the default rule, a bare custom_hue write is a gated-off
        # write: unsupported field, nothing changed.
        revision = await harness.wait_for_stable_revision(client)
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {
                    "module": "colorharmonizer",
                    "instance": 0,
                    "values": {},
                    "semantic_values": {"custom_hue": _vec(new_hues)},
                    "expected_revision": revision,
                },
            )
        assert excinfo.value.code == "unsupported_field"
        assert await _module_params(client, "colorharmonizer") == before

        # (b) node_saturation has no rule gate: it writes under the (still)
        # default rule.
        new_saturations = [0.5, 1.5, 0.2, 2.0]
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "colorharmonizer",
                "instance": 0,
                "values": {},
                "semantic_values": {"node_saturation": _vec(new_saturations)},
                "expected_revision": revision,
            },
        )
        _assert_values_equal(
            result["semantic_values"]["node_saturation"]["values"], new_saturations
        )

        # (c) The same custom_hue write composed with the rule switch in one
        # request applies: writability is evaluated against the projected
        # params (the colorbalance mode-switch precedent).
        revision = await harness.wait_for_stable_revision(client)
        result = await client.call(
            "set_module_params",
            {
                "module": "colorharmonizer",
                "instance": 0,
                "values": {"rule": "DT_COLORHARMONIZER_CUSTOM"},
                "semantic_values": {"custom_hue": _vec(new_hues)},
                "expected_revision": revision,
            },
        )
        assert result["values"]["rule"] == "DT_COLORHARMONIZER_CUSTOM"
        _assert_values_equal(result["semantic_values"]["custom_hue"]["values"], new_hues)

        params = await _module_params(client, "colorharmonizer")
        _assert_values_equal(params["semantic_values"]["custom_hue"]["values"], new_hues)
        _assert_values_equal(
            params["semantic_values"]["node_saturation"]["values"], new_saturations
        )


# ---------------------------------------------------------------------------
# 11. mixed scalar + vector request is one history item
# ---------------------------------------------------------------------------


async def test_negadoctor_scalar_and_vector_is_one_history_item(darktable_session):
    """A negadoctor `dmin` write plus a scalar `gamma` write in one call
    lands as exactly one history item (the m5 precedent for mixed
    scalar/semantic patches)."""
    async with harness.connected_client(darktable_session) as client:
        # Break same-module coalescing with gate 9's negadoctor edit, so
        # "exactly one new item" is attributable to this patch.
        await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.15}},
        )
        await harness.wait_for_stable_revision(client)
        history_before = await client.call("get_history", {"limit": 50})

        await client.call(
            "set_module_params",
            {
                "module": "negadoctor",
                "instance": 0,
                "values": {"gamma": 3.5},
                "semantic_values": {"dmin": _vec([0.95, 0.55, 0.35])},
            },
        )
        await harness.wait_for_stable_revision(client)
        history_after = await client.call("get_history", {"limit": 50})

        assert len(history_after["items"]) == len(history_before["items"]) + 1
        assert history_after["items"][-1]["op"] == "negadoctor"

        # Leave the shared session monotone for later files: negadoctor's
        # film-negative inversion (enabled by the render-diff gate above)
        # flips the exposure -> brightness relationship that
        # test_visual.py's histogram gate asserts, so switch the module
        # back off now that the ride-along gates are done.
        await client.call(
            "set_module_enabled",
            {"module": "negadoctor", "instance": 0, "enabled": False},
        )
        await harness.wait_for_stable_revision(client)
