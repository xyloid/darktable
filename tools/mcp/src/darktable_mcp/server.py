"""MCP tool surface for the darktable remote-edit protocol.

This is the only file in the package allowed to import the `mcp` SDK
(`discovery.py`, `protocol.py`, and `errors.py` stay SDK-free so the
transport client is reusable outside an MCP context). It exposes four
read-only tools, named per the implementation plan:

    get_current_image, list_modules, get_module_schema, get_module_params

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
    "Read-only introspection into a running darktable darkroom session: "
    "current image/view, live processing modules, and their parameter "
    "schemas and values. All tools require a darktable instance running "
    "with remote control enabled and, for darkroom-scoped tools, an image "
    "open in the darkroom."
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

    return app
