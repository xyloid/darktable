"""darktable_mcp: a Python client and MCP sidecar for darktable's private
remote-edit protocol.

`discovery`, `protocol`, and `errors` have no dependency on the `mcp` SDK
and are safe to import on their own (e.g. from a test or a plain script).
`build_server()` (in `server.py`) is the one entry point that touches the
SDK; import it explicitly (`from darktable_mcp.server import
build_server`) rather than from this top-level package, so importing the
transport client alone never requires `mcp` to be installed.

See README.md for the package layout and
docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md for
the wire contract.
"""

__version__ = "0.1.0"

__all__ = ["__version__"]
