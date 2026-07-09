# darktable-mcp

An [MCP](https://modelcontextprotocol.io) sidecar for darktable's
private remote-edit protocol. It lets an MCP client (an LLM agent, the
[MCP inspector](https://github.com/modelcontextprotocol/inspector), etc.)
introspect *and* edit a running darktable darkroom session: read the
current image, the live processing-module stack, and each module's
parameter schema and current values; mutate the edit by enabling or
resetting a module, creating a new module instance, inspecting the history
stack, and undoing the last change; and render a bounded JPEG preview of
the current edit state as native MCP image content.

This is the sidecar for plan step 5 of
`docs/superpowers/plans/2026-07-05-darktable-mcp-implementation-plan.md`.
It talks to darktable over the authenticated, loopback-only, framed-JSON
protocol implemented in `src/control/remote_*.c` (steps 1-4 of the same
plan); the full wire contract is
`docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`.

## Layout

```text
src/darktable_mcp/
  discovery.py   locate/validate session-<pid>.json discovery records
  protocol.py    framed-JSON client: connect, hello, correlate, reconnect
  errors.py      wire error codes + MCP-facing hints; no SDK dependency
  server.py      MCP tool definitions (the only file that imports `mcp`)
  __main__.py    `python -m darktable_mcp` / `darktable-mcp` stdio entry point
```

`discovery.py`, `protocol.py`, and `errors.py` have no dependency on the
`mcp` SDK and can be imported/used/tested on their own. `server.py` is the
one adapter layer on top.

## Tools exposed

| MCP tool | wire method | notes |
|---|---|---|
| `get_current_image` | `get_state` | view name + image metadata (`null` if none open) + revision |
| `list_modules` | `list_modules` | live module instances for the open image, in pixelpipe order |
| `get_module_schema` | `get_module_schema` | field types/ranges/enum values/writability for one op |
| `get_module_params` | `get_module_params` | current values for one module instance |
| `set_module_enabled` | `set_module_enabled` | turn a module on/off; one history item + new revision |
| `reset_module` | `reset_module` | reset a module to its defaults; returns post-reset values |
| `create_module_instance` | `create_module_instance` | duplicate a module into a new instance |
| `get_history` | `get_history` | the active edit-history stack (metadata only, no param blobs) |
| `undo` | `undo` | compare-and-undo the last change (requires `expected_revision`) |
| `render_preview` | `render_preview` | bounded JPEG preview of the current edit, as native image content |

All ten require darktable to be running with
`security/enable_remote_control` set to true. The darkroom-scoped tools
(everything except `get_current_image`) fail with a `not_in_darkroom` or
`no_image_open` error (surfaced as an MCP tool error with an actionable
hint, see `errors.py`) unless an image is currently open in the darkroom.

The four read tools (`get_current_image`, `list_modules`,
`get_module_schema`, `get_module_params`) never change the edit, and
neither does `render_preview` (it renders server-side on a background job
and returns the JPEG as an MCP image content block -- never base64 text
for the model; `max_px` is clamped to [64, 2048], `quality` to [50, 95],
and a `request_too_large` error means retry with a smaller `max_px`). The
mutating tools (`set_module_enabled`, `reset_module`,
`create_module_instance`, `undo`) advance the session revision; each
mutating call may take an `expected_revision` for compare-and-swap so a
stale client can't clobber a concurrent edit. Other protocol methods
documented in the reference (`set_module_params`, `compute_scopes`, ...)
are exposed elsewhere or reserved for a later step.

## Setup

Requires Python >= 3.10.

```sh
cd tools/mcp
python3 -m venv .venv
. .venv/bin/activate
pip install -e '.[dev]'
```

## Running the tests

The unit tests are self-contained: they run a fake framed-JSON server
in-process (plain `asyncio` streams on loopback) and never require a real
darktable instance.

```sh
cd tools/mcp
. .venv/bin/activate
pytest
```

`tests/test_protocol.py` reuses the shared request/response fixtures from
the C dispatcher's own tests
(`src/tests/unittests/control/fixtures/*.json`) so the two sides of the
wire contract are checked against the same example payloads. Those tests
locate the fixtures by walking up from `tests/` to the repository root
(`tools/mcp/tests/../../../src/tests/unittests/control/fixtures`), so they
only pass when run from inside a checkout of this repository -- not from
an installed copy of this package.

## Running against a real darktable instance

1. Enable remote control and point darktable at a scratch config directory
   (so it doesn't touch your real profile):

   ```sh
   mkdir -p /tmp/dt-mcp-check
   echo 'security/enable_remote_control=TRUE' > /tmp/dt-mcp-check/darktablerc
   build/bin/darktable --configdir /tmp/dt-mcp-check
   ```

2. Wait for `/tmp/dt-mcp-check/mcp/session-<pid>.json` to appear.
3. Point the sidecar at that config directory (or the exact record):

   ```sh
   darktable-mcp --config-dir /tmp/dt-mcp-check
   # or: darktable-mcp --discovery-path /tmp/dt-mcp-check/mcp/session-<pid>.json
   ```

4. Drive it with any MCP stdio client, e.g. the
   [MCP inspector](https://github.com/modelcontextprotocol/inspector), or
   in-process by calling `darktable_mcp.server.build_server(...)` and
   using its `call_tool()`/`list_tools()` methods directly (useful for
   scripted smoke checks without a full MCP client).

If no image is open in the darkroom, `list_modules` and
`get_module_params` return a `not_in_darkroom` (lighttable/other view) or
`no_image_open` (darkroom view, nothing loaded) tool error -- that is
expected, not a bug in the sidecar.

## Discovery record selection

Per the design spec, in order:

1. an explicit `--discovery-path` (or `discovery_path=` argument to
   `build_server()`/`discovery.select_record()`);
2. an explicit `--pid`;
3. otherwise, the newest live session under `<config-dir>/mcp/`.

A record is "live" if its JSON schema is well-formed and its `pid` is a
running process; see `discovery.is_process_alive()`'s docstring for a
Windows-specific caveat (`os.kill(pid, 0)` is unsafe there -- it would
call `TerminateProcess`, not merely probe -- so a `ctypes`-based
`OpenProcess`/`GetExitCodeProcess` check is used instead).

## Reconnection and timeouts

`protocol.ProtocolClient` opens a connection lazily on the first call and
keeps it open across calls. If a request's outcome is genuinely
ambiguous -- the connection dropped or the deadline passed while a
response was in flight -- the client never retries that request
automatically (darktable may or may not have processed it); it raises
`errors.RequestOutcomeUnknown` and drops the dead connection so the *next*
call reconnects cleanly. For the read tools, replaying a failed call by
hand is always safe; for the mutating tools (`set_module_enabled`,
`reset_module`, `create_module_instance`, `undo`) this matters more, since
a blind retry would risk double-applying an edit -- which is exactly why
those methods take an `expected_revision` for compare-and-swap.
