"""MCP tool surface for the darktable remote-edit protocol.

This is the only file in the package allowed to import the `mcp` SDK
(`discovery.py`, `protocol.py`, and `errors.py` stay SDK-free so the
transport client is reusable outside an MCP context). It exposes the
read-only introspection tools, named per the implementation plan:

    get_current_image, list_modules, get_module_schema, get_module_params

plus the darkroom mutation tools (plan steps 7-8):

    set_module_params, set_module_enabled, reset_module,
    create_module_instance, get_history, undo

plus the bounded preview renderer (plan step 9):

    render_preview

plus the scope analyzer (plan step 10):

    get_scopes

plus drawn-mask shape tools:

    list_mask_shapes, create_mask_shape, update_mask_shape,
    delete_mask_shape, set_mask_attachment

Each tool is a thin shape-conversion layer over one wire method call
through `protocol.ProtocolClient`; wire results are already compact and
model-oriented (per the protocol reference), so most tools return the wire
`result` object close to verbatim -- except `render_preview`, which
decodes the wire's base64 JPEG into a FastMCP `Image` so the model
receives native image content, never a base64 text blob, and `get_scopes`,
which returns MIXED content: one JSON text block with the numeric scope
data (revision, profile, histogram statistics, per-image metadata) plus a
native image block per rendered scope. Errors from the transport/protocol layer
propagate as exceptions; the MCP SDK's low-level dispatcher (see
`mcp.server.lowlevel.server.Server.call_tool`) catches any exception raised
from a tool function and turns it into an `isError` tool result carrying
`str(exception)` -- which is why `errors.ProtocolError.__str__` embeds the
actionable hint rather than requiring callers to fish it out separately.
"""

from __future__ import annotations

import base64
import json
import math
from typing import Any

from mcp.server.fastmcp import FastMCP, Image
from mcp.server.fastmcp.exceptions import ToolError

from . import discovery
from .errors import TransportError
from .protocol import ProtocolClient

SERVER_NAME = "darktable-mcp"
SERVER_INSTRUCTIONS = (
    "Inspect and edit a running darktable darkroom session: read the "
    "current image/view, live processing modules, and their parameter "
    "schemas and values; mutate the edit -- enable/disable and reset "
    "modules, create new module instances, read the history stack, and "
    "undo; list, create, update, delete, and attach drawn-mask shapes; "
    "and render a bounded JPEG preview of the current edit state -- "
    "or, when the `mask_render` capability is available, an individual "
    "module's blend mask via `render_preview`'s `show_mask`. "
    "All tools require a darktable instance running with remote control "
    "enabled and, for darkroom-scoped tools, an image open in the "
    "darkroom. Mutations accept an optional `expected_revision` for "
    "compare-and-swap against concurrent user edits. `get_scopes` returns "
    "photographic scope analysis (histogram statistics, waveform / RGB "
    "parade / vectorscope images) of the current darkroom preview."
)

# The four scope names compute_scopes accepts, in stable wire order.
_SCOPE_NAMES = ("histogram", "waveform", "parade", "vectorscope")

# The interpolation names the wire accepts for semantic curve patches
# (docs/superpowers/specs: curve design §Serialization rules).
_INTERPOLATIONS = {"CUBIC_SPLINE", "CATMULL_ROM", "MONOTONE_HERMITE"}


def _wire_semantic_values(curves: dict[str, Any]) -> dict[str, Any]:
    """Translates the tool-side `curves` shape (point pairs, any-case
    interpolation names) into the wire's `semantic_values` member
    (`class: "curve"`, `{x, y}` point objects, upper-case interpolation).
    Pure and connection-free; raises `ToolError` on an interpolation
    name the wire would reject, so the mistake never costs a round trip.
    """
    out: dict[str, Any] = {}
    for name, spec in curves.items():
        entry: dict[str, Any] = {
            "class": "curve",
            "points": [{"x": x, "y": y} for x, y in spec["points"]],
        }
        interp = spec.get("interpolation")
        if interp is not None:
            interp = str(interp).upper()
            if interp not in _INTERPOLATIONS:
                raise ToolError(
                    f"unknown interpolation {interp!r}; expected one of "
                    + ", ".join(sorted(_INTERPOLATIONS))
                )
            entry["interpolation"] = interp
        out[name] = entry
    return out


def _wire_vector_values(vectors: dict[str, Any]) -> dict[str, Any]:
    """Translate the `vectors` tool argument to wire semantic_values
    entries. Mirrors _wire_semantic_values: validate fully before any
    wire traffic."""
    out: dict[str, Any] = {}
    for name, values in vectors.items():
        ok = (isinstance(values, (list, tuple)) and len(values) > 0
              and all(isinstance(v, (int, float)) and not isinstance(v, bool)
                      and math.isfinite(v) for v in values))
        if not ok:
            raise ToolError(
                f"vector '{name}' must be a non-empty list of finite numbers")
        out[name] = {"class": "vector", "values": [float(v) for v in values]}
    return out


def _wire_band_values(bands: dict[str, Any]) -> dict[str, Any]:
    """Translate the `bands` tool argument to wire semantic_values
    entries. Tool shape: {name: {"y": [...], "x": [...]?}}. Mirrors
    _wire_vector_values: validate fully before any wire traffic, raise
    ToolError on bad shapes."""

    def _finite_samples(name: str, member: str, samples: Any) -> list[float]:
        ok = (isinstance(samples, (list, tuple)) and len(samples) > 0
              and all(isinstance(v, (int, float)) and not isinstance(v, bool)
                      and math.isfinite(v) for v in samples))
        if not ok:
            raise ToolError(
                f"band '{name}' member '{member}' must be a non-empty list "
                "of finite numbers")
        return [float(v) for v in samples]

    out: dict[str, Any] = {}
    for name, spec in bands.items():
        if not isinstance(spec, dict):
            raise ToolError(
                f"band '{name}' must be a dict with a 'y' sample list "
                "and an optional 'x' list")
        unknown = set(spec) - {"y", "x"}
        if unknown:
            raise ToolError(
                f"band '{name}' has unknown member(s): "
                + ", ".join(sorted(unknown)))
        entry: dict[str, Any] = {
            "class": "bands",
            "y": _finite_samples(name, "y", spec.get("y")),
        }
        x = spec.get("x")
        if x is not None:
            entry["x"] = _finite_samples(name, "x", x)
            if len(entry["x"]) != len(entry["y"]):
                raise ToolError(
                    f"band '{name}' x length {len(entry['x'])} does not "
                    f"match y length {len(entry['y'])}")
        out[name] = entry
    return out


def _wire_quantity_values(quantities: dict[str, Any]) -> dict[str, Any]:
    """Translate the `quantities` tool argument to wire semantic_values
    entries. Tool shape: {name: {component: number}}. Mirrors
    _wire_band_values: validate fully before any wire traffic, raise
    ToolError on bad shapes."""
    out: dict[str, Any] = {}
    for name, spec in quantities.items():
        if not isinstance(spec, dict) or not spec:
            raise ToolError(
                f"quantity '{name}' must be a non-empty dict mapping "
                "component names to finite numbers")
        values: dict[str, float] = {}
        for component, value in spec.items():
            ok = (isinstance(component, str)
                  and isinstance(value, (int, float))
                  and not isinstance(value, bool)
                  and math.isfinite(value))
            if not ok:
                raise ToolError(
                    f"quantity '{name}' must map component names to "
                    "finite numbers")
            values[component] = float(value)
        out[name] = {"class": "quantity", "values": values}
    return out


def build_server(
    *,
    discovery_path: str | None = None,
    pid: int | None = None,
    config_dir: str | None = None,
    connect_timeout: float = 5.0,
    request_timeout: float = 10.0,
) -> FastMCP:
    """Build the FastMCP server with its darkroom, preview, scope, and
    drawn-mask tools. Discovery is resolved lazily, on the first tool call
    -- not at build time -- so the process can start (and, importantly,
    can respond to MCP's `initialize` handshake) before a darktable
    instance necessarily exists yet.
    """
    app: FastMCP = FastMCP(SERVER_NAME, instructions=SERVER_INSTRUCTIONS)

    # Mutable holder rather than a bare nonlocal, so a future tool (or a
    # test) can peek at/reset the live client without reaching into
    # closures.
    state: dict[str, ProtocolClient] = {}

    async def _client() -> ProtocolClient:
        client = state.get("client")
        if client is None:
            record = discovery.select_record(
                discovery_path=discovery_path, pid=pid, config_dir=config_dir
            )
            client = ProtocolClient(
                record, connect_timeout=connect_timeout, request_timeout=request_timeout
            )
            state["client"] = client
        return client

    @app.tool()
    async def get_current_image() -> dict[str, Any]:
        """Return the current darktable view name and, if an image is open
        in the darkroom, its id/filename/dimensions/EXIF and the coherent
        history revision. `image` is `null` when no image is open."""
        client = await _client()
        return await client.call("get_state")

    @app.tool()
    async def list_modules() -> dict[str, Any]:
        """List every live processing-module instance for the image
        currently open in the darkroom, in pixelpipe order, with each
        entry's op name, instance number, display name, and enabled
        state. Requires an image open in the darkroom."""
        client = await _client()
        return await client.call("list_modules")

    @app.tool()
    async def get_module_schema(module: str) -> dict[str, Any]:
        """Return the parameter schema for a processing module: its
        fields' types, ranges/enum values, defaults, and whether each
        field is writable. `module` is the internal op name (e.g.
        "exposure"), the same string `list_modules` reports as `op`. The
        schema is per-op, not per-instance."""
        client = await _client()
        return await client.call("get_module_schema", {"module": module})

    @app.tool()
    async def get_module_params(module: str, instance: int = 0) -> dict[str, Any]:
        """Return the current parameter values of one module instance for
        the image open in the darkroom. `module` is the internal op name
        (e.g. "exposure"); `instance` is the module's `multi_priority`
        (0 for the default/only instance -- see `list_modules`)."""
        client = await _client()
        return await client.call("get_module_params", {"module": module, "instance": instance})

    @app.tool()
    async def set_module_params(
        module: str,
        values: dict[str, Any],
        instance: int = 0,
        enable: bool | None = None,
        expected_revision: int | None = None,
        curves: dict[str, Any] | None = None,
        vectors: dict[str, Any] | None = None,
        bands: dict[str, Any] | None = None,
        quantities: dict[str, Any] | None = None,
        blend: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Set parameter values on one module instance, recorded as one
        history step. `module` is the internal op name, `instance` its
        `multi_priority`, and `values` maps writable field names (dotted
        introspection names -- see `get_module_schema`) to new values;
        enum fields accept their stable member name or integer value. The
        patch is atomic: any invalid field fails the whole request with
        `invalid_value`/`unknown_field`, changing nothing. Setting params
        does not enable a disabled module; pass `enable` true to switch it
        on in the same history step. Pass `expected_revision` for
        compare-and-swap (see `set_module_enabled`). Returns the module,
        instance, resulting `enabled` state, the `values` read back from
        live state, and the new `revision`.

        `curves` edits semantic curve parameters (see `get_module_schema`'s
        `semantic_fields`, e.g. rgbcurve's `curve.master`) and needs a
        darktable that advertises the `curve_params` capability. It maps
        semantic IDs to `{"points": [[x, y], ...], "interpolation"?:
        "cubic_spline" | "catmull_rom" | "monotone_hermite"}`. Curve
        invariants: 2-20 points; x strictly ascending with adjacent
        points more than 0.0025 apart; coordinates are the module's
        stored (pre-display) space in [0, 1]; each patch replaces that
        whole curve (unlisted curves are untouched); omitted
        interpolation keeps the curve's current one. The response's
        `semantic_values` reads back every written curve with points as
        `{x, y}` objects.

        `vectors` edits semantic vector parameters (see
        `get_module_schema`'s `semantic_fields` with `"class": "vector"`,
        e.g. colorbalance's `lift`) and needs a darktable that
        advertises the `vector_params` capability. It maps semantic IDs to
        a flat list of finite numbers; each patch replaces that whole
        named vector (unlisted vectors are untouched). Components are the
        module's stored-space values -- e.g. colorbalance stores its
        identity lift/gamma/gain as 1.0, not 0.0.

        `bands` edits semantic sampled-response parameters (see
        `get_module_schema`'s `semantic_fields` with `"class": "bands"`,
        e.g. atrous's `bands.luma`) and needs a darktable that advertises
        the `band_params` capability. It maps semantic IDs to
        `{"y": [samples], "x"?: [positions]}`. `y` is required and
        replaces that whole band (exactly `count` samples, each within the
        schema's `y_range`); `x` is accepted only on bands whose schema
        says `x_policy: "interior"` -- endpoints must equal the stored
        endpoints, positions strictly ascending with adjacent gaps of at
        least `min_gap`. A band whose schema names an `x_shared_with` twin
        mirrors any x write to that twin. Unlisted bands are untouched.

        `quantities` edits derived semantic quantities (see
        `get_module_schema`'s `semantic_fields` with `"class":
        "quantity"`, e.g. temperature's `wb.temperature`) and needs a
        darktable that advertises the `quantity_params` capability. It
        maps semantic IDs to `{component: number}` -- every component the
        schema lists, exactly once, each within its `minimum`/`maximum`
        (e.g. `{"temperature": 5500, "tint": 1.0}`). A quantity is
        derived: the module converts it to its native stored fields
        (the schema names them in `represented_by` -- for
        `wb.temperature`, temperature's `red`/`green`/`blue`/`various`
        coefficients), so do not also write those native fields in
        `values` in the same request -- that conflict is rejected
        server-side. Read-back is lossy: the response re-derives the
        quantity from what was actually stored, so it may differ
        slightly from what was written (float narrowing, conversion
        round trip).

        `blend` edits the module's blend settings (opacity, blend mode,
        blending colorspace, mask refinement) and needs a darktable that
        advertises the `blend_params` capability. Members: `mask_mode`
        (the schema gives the state-aware choice set:
        `off`/`uniform`/`parametric` are writable in non-drawn supported
        families; `drawn` and `drawn+parametric` may toggle only the
        parametric bit while a drawn mask stays attached; entering or
        leaving drawn ownership is attach/detach-only; raster is read-only),
        `colorspace` and `mode` (C enumerator names; see
        `get_module_schema`'s `blend` section for this instance's choice
        sets), `reverse` (bool), `fulcrum` (EV), `opacity` (0-100),
        `feathering_radius` (0-250 px), `feathering_guide`,
        `blur_radius` (0-100 px), `contrast`/`brightness`/`details`
        (-1..1; `details` needs a raw image). WARNING: writing
        `colorspace` deterministically resets `mode`, `reverse`,
        `fulcrum`, and any parametric-mask thresholds to the new space's
        defaults -- send replacement values in the same call if you want
        them. The response's `blend` member reads back the complete
        post-commit blend state.

        `blend.parametric` (capability `parametric_mask_params`) maps
        effective-colorspace channel names such as `Jz_in` to complete slot
        replacements: `{markers:[m0,m1,m2,m3], inverted?, boost?}`. Markers
        are ascending in [0,1]; [0,0,1,1] disables the slot, and null resets
        it. To select bright sky/highlights deterministically, set
        `colorspace:"DEVELOP_BLEND_CS_RGB_SCENE"`, `combine:"exclusive"`,
        `mask_mode:"parametric"`, and `Jz_in:{markers:[0.55,0.65,1,1]}` with
        omitted/false `inverted`. To select shadows, use
        `Jz_in:{markers:[0,0,0.2,0.35]}`. Boost is an exp2 exponent and does
        not rescale markers. Inclusive combine XORs effective polarity;
        changing combine and explicit inverted together requires
        `allow_inverted_combine:true`. Verify thresholds with `show_mask`.

        `curves`, `vectors`, `bands`, and `quantities` may be given
        together; a semantic ID given in more than one raises an error
        before anything is sent."""
        params: dict[str, Any] = {"module": module, "instance": instance, "values": values}
        if enable is not None:
            params["enable"] = enable
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        # Translate before connecting: a malformed `curves`/`vectors`/
        # `bands` argument (e.g. a bad interpolation name, a non-finite
        # sample) fails without any wire traffic.
        translated = {
            "curves": _wire_semantic_values(curves) if curves is not None else None,
            "vectors": _wire_vector_values(vectors) if vectors is not None else None,
            "bands": _wire_band_values(bands) if bands is not None else None,
            "quantities": _wire_quantity_values(quantities) if quantities is not None else None,
        }
        semantic_values: dict[str, Any] | None = None
        if any(entries is not None for entries in translated.values()):
            id_sources: dict[str, list[str]] = {}
            for argument, entries in translated.items():
                for semantic_id in entries or {}:
                    id_sources.setdefault(semantic_id, []).append(argument)
            overlap = sorted(
                semantic_id
                for semantic_id, sources in id_sources.items()
                if len(sources) > 1
            )
            if overlap:
                raise ToolError(
                    "semantic id(s) given in more than one of `curves`, "
                    "`vectors`, `bands`, `quantities`: " + ", ".join(overlap)
                )
            semantic_values = {
                semantic_id: entry
                for entries in translated.values()
                for semantic_id, entry in (entries or {}).items()
            }
        client = await _client()
        if semantic_values is not None:
            await client.ensure_connected()
            if curves is not None and "curve_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise curve_params; "
                    "upgrade darktable to edit curves"
                )
            if vectors is not None and "vector_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise vector_params; "
                    "upgrade darktable to edit vectors"
                )
            if bands is not None and "band_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise band_params; "
                    "upgrade darktable to edit bands"
                )
            if quantities is not None and "quantity_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise quantity_params; "
                    "upgrade darktable to edit quantities"
                )
            params["semantic_values"] = semantic_values
        if blend is not None:
            await client.ensure_connected()
            if "blend_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise blend_params; "
                    "upgrade darktable to edit blend settings"
                )
            # Tier 2 / M-B: the parametric/combine members and the three
            # blendif-owning mask modes (parametric, drawn, drawn+parametric),
            # plus the inverted+combine override, require the extra capability.
            tier2_mask_mode = blend.get("mask_mode") in {
                "parametric", "drawn", "drawn+parametric"
            }
            needs_parametric = tier2_mask_mode or any(
                key in blend
                for key in ("parametric", "combine", "allow_inverted_combine")
            )
            if needs_parametric and "parametric_mask_params" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise parametric_mask_params; "
                    "upgrade darktable to edit parametric masks"
                )
            params["blend"] = blend
        return await client.call("set_module_params", params)

    @app.tool()
    async def set_module_enabled(
        module: str,
        enabled: bool,
        instance: int = 0,
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Switch one module instance on or off -- the explicit equivalent
        of its darkroom on/off toggle, recorded as one history step.
        `module` is the internal op name, `instance` its `multi_priority`.
        Pass `expected_revision` (from a prior read) for compare-and-swap:
        the call fails with `revision_conflict` (retryable) if the darkroom
        history changed since then, changing nothing. Returns the module,
        instance, resulting `enabled` state, and new `revision`."""
        params: dict[str, Any] = {"module": module, "instance": instance, "enabled": enabled}
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        return await client.call("set_module_enabled", params)

    @app.tool()
    async def reset_module(
        module: str,
        instance: int = 0,
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Reset one module instance to its defaults through darktable's
        normal reset lifecycle (the same as the module's reset button),
        recorded as one history step. `module` is the internal op name,
        `instance` its `multi_priority`. Pass `expected_revision` for
        compare-and-swap (see `set_module_enabled`). Returns the module,
        instance, resulting `enabled` state, the post-reset `values`, and
        the new `revision`."""
        params: dict[str, Any] = {"module": module, "instance": instance}
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        return await client.call("reset_module", params)

    @app.tool()
    async def create_module_instance(
        module: str,
        source_instance: int = 0,
        copy_params: bool = False,
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Create a new instance of a multi-instance module, exactly like
        the darkroom's new-instance / duplicate button. `module` is the
        internal op name and `source_instance` the `multi_priority` of the
        instance to base it on. `copy_params` true duplicates the source's
        parameters; false yields defaults. A single-instance module fails
        with `instance_not_supported`, creating nothing. Pass
        `expected_revision` for compare-and-swap (see `set_module_enabled`).
        Returns the module, the new `instance` number, its `instance_name`,
        `enabled` state, and the new `revision`."""
        params: dict[str, Any] = {
            "module": module,
            "source_instance": source_instance,
            "copy_params": copy_params,
        }
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        return await client.call("create_module_instance", params)

    @app.tool()
    async def get_history(limit: int = 20) -> dict[str, Any]:
        """Return the darkroom edit-history stack as model-oriented
        metadata only (no parameter blobs -- use `get_module_params` for
        values). Each item carries its stack position `seq`, the module
        `op`, `instance`, translated `display_name`/`instance_name`, and
        `enabled` state, ordered oldest to newest. `limit` (default 20) is
        clamped to the server's [1, 100] window. Returns the current
        `revision` and the `items` list."""
        client = await _client()
        return await client.call("get_history", {"limit": limit})

    @app.tool()
    async def undo(expected_revision: int) -> dict[str, Any]:
        """Undo exactly one darkroom history transition through darktable's
        undo system (the same as Ctrl+Z). `expected_revision` is REQUIRED
        and enforced as compare-and-undo: the undo happens only if it
        matches the live revision, otherwise the call fails with
        `revision_conflict` (retryable) and nothing changes -- so read the
        current revision first and never undo a state you have not
        observed. Returns the new post-undo `revision`."""
        client = await _client()
        return await client.call("undo", {"expected_revision": expected_revision})

    @app.tool()
    async def list_mask_shapes() -> dict[str, Any]:
        """List drawn-mask shapes and their module memberships. Requires
        the `mask_shapes` capability."""
        client = await _client()
        await client.ensure_connected()
        if "mask_shapes" not in client.capabilities:
            raise TransportError(
                "this darktable does not advertise mask_shapes; "
                "upgrade darktable to manage drawn mask shapes"
            )
        return await client.call("list_mask_shapes")

    @app.tool()
    async def create_mask_shape(
        type: str,
        geometry: dict[str, Any],
        name: str | None = None,
        attach: dict[str, Any] | None = None,
        space: str = "preview",
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Create one drawn-mask shape. `type` and `geometry` must use a
        darktable editable shape type and its complete geometry members;
        `name` omitted means darktable chooses a name, and `attach` may
        create it already attached to a module. Coordinates are
        preview-normalized `[0,1]` over the rendered image (`space` defaults
        to `"preview"`): point centers/anchors map exactly; radii and borders
        are fractions of the shorter rendered edge; sizes and angles may be
        reported `approximate` under perspective correction in
        `size_mapping`. Editing a shared shape with `update_mask_shape`
        changes every module using it; that update's `affects_instances`
        reports how many. Requires `mask_shapes`."""
        params: dict[str, Any] = {
            "type": type,
            "geometry": geometry,
            "space": space,
        }
        if name is not None:
            params["name"] = name
        if attach is not None:
            params["attach"] = attach
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        await client.ensure_connected()
        if "mask_shapes" not in client.capabilities:
            raise TransportError(
                "this darktable does not advertise mask_shapes; "
                "upgrade darktable to manage drawn mask shapes"
            )
        return await client.call("create_mask_shape", params)

    @app.tool()
    async def update_mask_shape(
        id: int,
        geometry: dict[str, Any],
        name: str | None = None,
        space: str = "preview",
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Replace one shape's complete geometry; coordinates are as in
        `create_mask_shape` (preview-normalized `[0,1]`). A shared-shape
        update edits every module using it, reported as `affects_instances`;
        sizes/angles can be `approximate` under perspective in
        `size_mapping`. A gradient update preserves its existing GUI
        linear/sigmoidal transition mode. Requires `mask_shapes`."""
        params: dict[str, Any] = {"id": id, "geometry": geometry, "space": space}
        if name is not None:
            params["name"] = name
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        await client.ensure_connected()
        if "mask_shapes" not in client.capabilities:
            raise TransportError(
                "this darktable does not advertise mask_shapes; "
                "upgrade darktable to manage drawn mask shapes"
            )
        return await client.call("update_mask_shape", params)

    @app.tool()
    async def delete_mask_shape(
        id: int, expected_revision: int | None = None
    ) -> dict[str, Any]:
        """Delete a drawn-mask shape and its memberships. Requires
        `mask_shapes`."""
        params: dict[str, Any] = {"id": id}
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        await client.ensure_connected()
        if "mask_shapes" not in client.capabilities:
            raise TransportError(
                "this darktable does not advertise mask_shapes; "
                "upgrade darktable to manage drawn mask shapes"
            )
        return await client.call("delete_mask_shape", params)

    @app.tool()
    async def set_mask_attachment(
        op: str,
        shape_id: int,
        attached: bool,
        instance: int = 0,
        state: str | None = None,
        inverted: bool | None = None,
        opacity: float | None = None,
        expected_revision: int | None = None,
    ) -> dict[str, Any]:
        """Attach or detach a shape from one module instance. With
        `attached=true`, this upserts membership and applies
        `state`/`inverted`/`opacity`; with `attached=false`, it detaches but
        the shape survives if another module uses it. `state` is ignored
        when the shape becomes the bottom member of a module group (that
        member has no combine op); `inverted` and `opacity` still apply.
        Requires `mask_shapes`."""
        params: dict[str, Any] = {
            "op": op,
            "shape_id": shape_id,
            "attached": attached,
            "instance": instance,
        }
        if state is not None:
            params["state"] = state
        if inverted is not None:
            params["inverted"] = inverted
        if opacity is not None:
            params["opacity"] = opacity
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        client = await _client()
        await client.ensure_connected()
        if "mask_shapes" not in client.capabilities:
            raise TransportError(
                "this darktable does not advertise mask_shapes; "
                "upgrade darktable to manage drawn mask shapes"
            )
        return await client.call("set_mask_attachment", params)

    # Mixed content for the mask branch, so structured output is off: an
    # ordinary preview still returns a single native Image block; a mask
    # render returns a JSON metadata block plus the native Image (the
    # get_scopes mixed-content idiom, per design amendment 6).
    @app.tool(structured_output=False)
    async def render_preview(
        max_px: int = 1024,
        quality: int = 85,
        show_mask: dict[str, Any] | None = None,
    ) -> list[Any]:
        """Render the current darkroom image as a native JPEG content
        block. `max_px` is clamped to [64, 2048] and `quality` to [50, 95].

        `show_mask={"op": <module>, "instance": <n>}` instead renders that
        module's blend mask (white = full effect), with identical framing.
        It requires `mask_render`; off/uniform masks are solid white. Mask
        renders return a JSON metadata block (`mime_type`, dimensions,
        revision, `mask_of`) followed by the native JPEG block.
        """
        max_px = max(64, min(2048, max_px))
        quality = max(50, min(95, quality))
        params: dict[str, Any] = {"max_px": max_px, "quality": quality}
        client = await _client()
        if show_mask is not None:
            await client.ensure_connected()
            if "mask_render" not in client.capabilities:
                raise TransportError(
                    "this darktable does not advertise mask_render; "
                    "upgrade darktable to render masks"
                )
            params["show_mask"] = show_mask

        result = await client.call("render_preview", params)
        # Native MCP image content, never a base64 text blob to the model
        # (a plan step 9 binding requirement): decode the wire base64 and
        # hand FastMCP an Image, which becomes an ImageContent block.
        image = Image(data=base64.b64decode(result["data"]), format="jpeg")
        if show_mask is None:
            return [image]
        if not isinstance(result.get("mask_of"), dict):
            raise TransportError("darktable returned a mask image without mask_of metadata")
        metadata = {key: value for key, value in result.items() if key != "data"}
        return [json.dumps(metadata), image]

    # Mixed content (text + images), so structured output is explicitly off:
    # FastMCP converts each returned list element individually -- the JSON
    # string becomes a TextContent block, each Image an ImageContent block.
    @app.tool(structured_output=False)
    async def get_scopes(
        scopes: list[str] | None = None,
        include_summary: bool = True,
        include_bins: bool = False,
        include_images: bool = True,
        image_size: int = 512,
    ) -> list[Any]:
        """Analyze the current darkroom preview with darktable's
        photographic scopes and return the results the model can reason
        about directly. `scopes` selects any non-empty subset of
        ["histogram", "waveform", "parade", "vectorscope"] (default: all
        four). The first content block is JSON text carrying the numeric
        data: the `revision` the analysis derives from, `source`
        ("final_preview"), `color_profile`, `roi`, the histogram numeric
        summary (clip fractions, luminance percentiles, channel means) when
        `include_summary`, 256 normalized per-channel bins when
        `include_bins`, and each rendered scope's mime type and pixel
        dimensions. Rendered waveform / RGB parade / vectorscope plots
        follow as native image blocks (when `include_images`), in that
        order. `image_size` bounds the scope images (clamped to
        [128, 1024]). All scopes in one response derive from the same
        preview buffer and share one revision; if no preview has been
        computed yet the call fails with a retryable `scope_failed` --
        retry after darktable's preview updates."""
        if scopes is None:
            scopes = list(_SCOPE_NAMES)
        # The server clamps too (its contract); clamping here as well keeps
        # the request honest and self-describing on the wire.
        image_size = max(128, min(1024, image_size))
        client = await _client()
        result = await client.call(
            "compute_scopes",
            {
                "scopes": scopes,
                "include_summary": include_summary,
                "include_bins": include_bins,
                "include_images": include_images,
                "image_size": image_size,
            },
        )

        # Split the wire result into the numeric part (returned as one JSON
        # text block -- note M39: unlike render_preview, this tool returns
        # the revision/dimension data its docstring promises) and native
        # image blocks (never base64 text to the model).
        summary: dict[str, Any] = {
            key: value
            for key, value in result.items()
            if key not in ("waveform", "parade", "vectorscope")
        }
        images: list[Image] = []
        for name in ("waveform", "parade", "vectorscope"):
            entry = result.get(name)
            if not isinstance(entry, dict):
                continue
            img = entry.get("image")
            if not isinstance(img, dict):
                summary[name] = entry
                continue
            # metadata (mime type, dimensions) stays in the text block;
            # the pixel payload becomes a native image block
            summary[name] = {
                "image": {key: value for key, value in img.items() if key != "data"}
            }
            images.append(Image(data=base64.b64decode(img["data"]), format="png"))

        return [json.dumps(summary), *images]

    return app
