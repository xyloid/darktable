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

**New here?** Start with the user & host guide,
[`docs/remote-control.md`](docs/remote-control.md): how to enable remote
control, how to point an MCP host at darktable (with a `mcpServers` config
snippet), where discovery records live on each platform, how multiple
instances are selected, the security model, and the versioning policy.
For a command-by-command source build and Claude Code setup on Ubuntu, use
[`docs/ubuntu-claude-code-setup.md`](docs/ubuntu-claude-code-setup.md).

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
| `set_module_params` | `set_module_params` | atomically patch writable fields (plus semantic curves via `curves` and semantic vectors via `vectors`); one history item + new revision |
| `set_module_enabled` | `set_module_enabled` | turn a module on/off; one history item + new revision |
| `reset_module` | `reset_module` | reset a module to its defaults; returns post-reset values |
| `create_module_instance` | `create_module_instance` | duplicate a module into a new instance |
| `get_history` | `get_history` | the active edit-history stack (metadata only, no param blobs) |
| `undo` | `undo` | compare-and-undo the last change (requires `expected_revision`) |
| `render_preview` | `render_preview` | bounded JPEG preview of the current edit, as native image content |
| `get_scopes` | `compute_scopes` | histogram summaries/bins plus waveform, parade, and vectorscope images |

All twelve require darktable to be running with
`security/enable_remote_control` set to true. The darkroom-scoped tools
(everything except `get_current_image`) fail with a `not_in_darkroom` or
`no_image_open` error (surfaced as an MCP tool error with an actionable
hint, see `errors.py`) unless an image is currently open in the darkroom.

The state/schema reads, `get_history`, `render_preview`, and `get_scopes`
never change the edit. `render_preview` runs server-side on a background job
and returns the JPEG as an MCP image content block -- never base64 text for
the model; `max_px` is clamped to [64, 2048], `quality` to [50, 95], and a
`request_too_large` error means retry with a smaller `max_px`. The
mutating tools (`set_module_params`, `set_module_enabled`, `reset_module`,
`create_module_instance`, `undo`) advance the session revision; each
mutating call may take an `expected_revision` for compare-and-swap so a
stale client can't clobber a concurrent edit. `set_module_params` is atomic
(any invalid field fails the whole patch, changing nothing) and never
enables a disabled module implicitly -- pass `enable: true` to switch it on
in the same history step.

`set_module_params` also edits semantic curve parameters through its
optional `curves` argument: semantic IDs mapped to `{"points": [[x, y],
...], "interpolation"?: "cubic_spline" | "catmull_rom" |
"monotone_hermite"}`. Four modules currently expose curve semantics —
`rgbcurve` (`curve.master`/`curve.red`/`curve.green`/`curve.blue`),
`tonecurve` (`curve.lightness`, plus `curve.a`/`curve.b` in independent-Lab
mode), `colorzones` (`curve.lightness`/`curve.chroma`/`curve.hue`), and
`basecurve` (`curve.master`) — and `get_module_schema`'s `semantic_fields`
is the authoritative source of each module's curve IDs. This requires a
darktable that advertises the `curve_params` hello capability; against an
older darktable the tool refuses client-side with an upgrade message
instead of silently dropping the curve half of a patch.

`set_module_params` also edits semantic vector parameters (milestone 4)
through its optional `vectors` argument: semantic IDs mapped to a flat
list of finite numbers, e.g. `{"lift": [1.0, 1.1, 1.0, 0.95]}`. Each patch
replaces the whole named vector; unlisted vectors are untouched. Five
modules currently expose vector semantics — `colorbalance` (`lift`/
`gamma`/`gain`, with mode-gated aliases `offset`/`power`/`slope` writable
instead under the module's default `SLOPE_OFFSET_POWER` mode),
`channelmixerrgb` (`red`/`green`/`blue`/`saturation`/`lightness`/`grey`
mixing rows), `rgblevels` (`levels.linked` when `autoscale` is linked,
`levels.red`/`levels.green`/`levels.blue` when it is independent),
`borders` (`color`/`frame_color`), and `watermark` (`color`) — and
`get_module_schema`'s `semantic_fields` (`"class": "vector"`) is the
authoritative source of each module's vector IDs, component count, and
per-component ranges. This requires a darktable that advertises the
`vector_params` hello capability; against an older darktable the tool
refuses client-side with an upgrade message instead of silently dropping
the vector half of a patch. `curves` and `vectors` may be given together
in the same call; a semantic ID given in both raises a client-side error
before either is sent.

**Stored-value warning.** Vector components are the module's *stored*
values, not what the GUI displays. `colorbalance` is the sharp case: its
identity lift/gamma/gain is stored as `1.0` for every component, which the
GUI renders as `0.0` (the R/G/B components) or `0%` (the factor
component) — sending `0.0` to "reset" a component actually drives it hard
away from identity, not toward it. A SOP-mode (the module's default)
colorbalance write:

```json
{
  "module": "colorbalance",
  "values": {},
  "vectors": {
    "offset": [1.1, 1.05, 0.95, 1.0],
    "power":  [0.9, 1.0, 1.0, 1.1],
    "slope":  [1.05, 0.9, 1.1, 1.0]
  }
}
```

writes `offset`/`power`/`slope` — `SLOPE_OFFSET_POWER` mode's names for
the same `lift`/`gamma`/`gain` storage. A single call that also sets
`"values": {"mode": "LIFT_GAMMA_GAIN"}` and patches `lift`/`gamma`/`gain`
instead lands atomically: `writable_when` is checked against the
*projected* params, so the mode switch and the newly-active names apply
together in one history step.

`rgblevels` shows the linked/independent alias gating: with the module's
default `autoscale` (`DT_IOP_RGBLEVELS_LINKED_CHANNELS`) only
`levels.linked` is writable; switching `autoscale` to
`DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS` in the same request makes
`levels.red`/`levels.green`/`levels.blue` writable instead, and writing
one row never resets the others — the unwritten rows read back at their
prior stored values, not defaults:

```json
{
  "module": "rgblevels",
  "values": { "autoscale": "DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS" },
  "vectors": { "levels.red": [0.05, 0.4, 0.95] }
}
```

Each `levels.*` vector is a `[black, grey, white]` triple; the schema's
`ordering` constraint enforces `black < grey < white` with a minimum gap.

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
