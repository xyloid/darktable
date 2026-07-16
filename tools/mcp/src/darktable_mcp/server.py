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
    "undo; and render a bounded JPEG preview of the current edit state. "
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
    Pure and connection-free; raises `ValueError` on an interpolation
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
                raise ValueError(
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


def build_server(
    *,
    discovery_path: str | None = None,
    pid: int | None = None,
    config_dir: str | None = None,
    connect_timeout: float = 5.0,
    request_timeout: float = 10.0,
) -> FastMCP:
    """Builds the FastMCP server instance with all four read-only tools
    registered. Discovery is resolved lazily, on the first tool call --
    not at build time -- so the process can start (and, importantly, can
    respond to MCP's `initialize` handshake) before a darktable instance
    necessarily exists yet.
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
        e.g. colorbalancergb's `lift`) and needs a darktable that
        advertises the `vector_params` capability. It maps semantic IDs to
        a flat list of finite numbers; each patch replaces that whole
        named vector (unlisted vectors are untouched). Components are the
        module's stored-space values -- e.g. colorbalancergb stores its
        identity lift/gamma/gain as 1.0, not 0.0. `curves` and `vectors`
        may be given together; a semantic ID given in both raises an
        error before either is sent."""
        params: dict[str, Any] = {"module": module, "instance": instance, "values": values}
        if enable is not None:
            params["enable"] = enable
        if expected_revision is not None:
            params["expected_revision"] = expected_revision
        # Translate before connecting: a malformed `curves`/`vectors`
        # argument (e.g. a bad interpolation name, a non-finite vector
        # component) fails without any wire traffic.
        curve_semantic_values = _wire_semantic_values(curves) if curves is not None else None
        vector_semantic_values = _wire_vector_values(vectors) if vectors is not None else None
        semantic_values: dict[str, Any] | None = None
        if curve_semantic_values is not None or vector_semantic_values is not None:
            curve_semantic_values = curve_semantic_values or {}
            vector_semantic_values = vector_semantic_values or {}
            overlap = set(curve_semantic_values) & set(vector_semantic_values)
            if overlap:
                raise ToolError(
                    "semantic id(s) given in both `curves` and `vectors`: "
                    + ", ".join(sorted(overlap))
                )
            semantic_values = {**curve_semantic_values, **vector_semantic_values}
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
            params["semantic_values"] = semantic_values
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
    async def render_preview(max_px: int = 1024, quality: int = 85) -> Image:
        """Render the image currently open in the darkroom, with its full
        current edit history and normal output color management (sRGB),
        and return it as a JPEG image the model can look at directly.
        `max_px` bounds the longest edge (clamped to [64, 2048], default
        1024); `quality` is the JPEG quality (clamped to [50, 95], default
        85). Rendering happens asynchronously in darktable and can take a
        few seconds for large raws. If it fails with `request_too_large`,
        retry with a smaller `max_px`. The wire response's `revision` says
        which history state was rendered."""
        # The server clamps too (its contract); clamping here as well keeps
        # the request honest and self-describing on the wire.
        max_px = max(64, min(2048, max_px))
        quality = max(50, min(95, quality))
        client = await _client()
        result = await client.call("render_preview", {"max_px": max_px, "quality": quality})
        # Native MCP image content, never a base64 text blob to the model
        # (a plan step 9 binding requirement): decode the wire base64 and
        # hand FastMCP an Image, which becomes an ImageContent block.
        return Image(data=base64.b64decode(result["data"]), format="jpeg")

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
