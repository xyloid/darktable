"""MCP tool surface for the darktable remote-edit protocol.

This is the only file in the package allowed to import the `mcp` SDK
(`discovery.py`, `protocol.py`, and `errors.py` stay SDK-free so the
transport client is reusable outside an MCP context). It exposes the
read-only introspection tools, named per the implementation plan:

    get_current_image, list_modules, get_module_schema, get_module_params

plus the darkroom mutation tools (plan steps 7-8):

    set_module_enabled, reset_module, create_module_instance,
    get_history, undo

Each tool is a thin shape-conversion layer over one wire method call
through `protocol.ProtocolClient`; wire results are already compact and
model-oriented (per the protocol reference), so most tools return the wire
`result` object close to verbatim. Errors from the transport/protocol layer
propagate as exceptions; the MCP SDK's low-level dispatcher (see
`mcp.server.lowlevel.server.Server.call_tool`) catches any exception raised
from a tool function and turns it into an `isError` tool result carrying
`str(exception)` -- which is why `errors.ProtocolError.__str__` embeds the
actionable hint rather than requiring callers to fish it out separately.
"""

from __future__ import annotations

from typing import Any

from mcp.server.fastmcp import FastMCP

from . import discovery
from .protocol import ProtocolClient

SERVER_NAME = "darktable-mcp"
SERVER_INSTRUCTIONS = (
    "Inspect and edit a running darktable darkroom session: read the "
    "current image/view, live processing modules, and their parameter "
    "schemas and values; and mutate the edit -- enable/disable and reset "
    "modules, create new module instances, read the history stack, and "
    "undo. All tools require a darktable instance running with remote "
    "control enabled and, for darkroom-scoped tools, an image open in the "
    "darkroom. Mutations accept an optional `expected_revision` for "
    "compare-and-swap against concurrent user edits."
)


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

    return app
