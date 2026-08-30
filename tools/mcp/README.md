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
For a command-by-command source build and Claude Code or Codex setup on
Ubuntu, use
[`docs/ubuntu-mcp-client-setup.md`](docs/ubuntu-mcp-client-setup.md).
That guide builds with `--disable-ai`; if you drop that flag to use the
AI modules, [`docs/ai-build-tree-notes.md`](docs/ai-build-tree-notes.md)
covers the two things a build tree lacks that an install would have set
up, both of which fail at runtime with the same unhelpful message.
Once it is connected, [Agent skill](#agent-skill) below installs the
skill that teaches an agent to actually edit well with these tools.

This is the sidecar for plan step 5 of
`docs/superpowers/plans/2026-07-05-darktable-mcp-implementation-plan.md`.
It talks to darktable over the authenticated, loopback-only, framed-JSON
protocol implemented in `src/control/remote_*.c` (steps 1-4 of the same
plan); the full wire contract is
`docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`.

## Layout

```text
env.sh           source to set up the dev environment (see Setup below)
src/darktable_mcp/
  discovery.py   locate/validate session-<pid>.json discovery records
  protocol.py    framed-JSON client: connect, hello, correlate, reconnect
  errors.py      wire error codes + MCP-facing hints; no SDK dependency
  server.py      MCP tool definitions (the only file that imports `mcp`)
  __main__.py    `python -m darktable_mcp` / `darktable-mcp` stdio entry point
skills/
  darktable-editing/  agent skill teaching an agent to drive these tools
                      well (see Agent skill below)
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
| `set_module_params` | `set_module_params` | atomically patch writable fields (plus semantic curves via `curves`, semantic vectors via `vectors`, semantic bands via `bands`, and semantic quantities via `quantities`); one history item + new revision |
| `set_module_enabled` | `set_module_enabled` | turn a module on/off; one history item + new revision |
| `reset_module` | `reset_module` | reset a module to its defaults; returns post-reset values |
| `create_module_instance` | `create_module_instance` | duplicate a module into a new instance |
| `get_history` | `get_history` | the active edit-history stack (metadata only, no param blobs) |
| `undo` | `undo` | compare-and-undo the last change (requires `expected_revision`) |
| `render_preview` | `render_preview` | bounded JPEG preview of the current edit, as native image content |
| `get_scopes` | `compute_scopes` | histogram summaries/bins plus waveform, parade, and vectorscope images |
| `list_mask_shapes` | `list_mask_shapes` | list drawn shapes and their transitive module memberships |
| `create_mask_shape` | `create_mask_shape` | create a circle, ellipse, or gradient, optionally attached to a module |
| `update_mask_shape` | `update_mask_shape` | replace one editable shape's complete geometry or name |
| `delete_mask_shape` | `delete_mask_shape` | delete one shape and its memberships transitively |
| `set_mask_attachment` | `set_mask_attachment` | attach, update, or detach one shape from a module instance |

All 17 require darktable to be running with
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
`create_module_instance`, `undo`, `create_mask_shape`, `update_mask_shape`,
`delete_mask_shape`, and state-changing `set_mask_attachment`) advance the
session revision; each
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
replaces the whole named vector; unlisted vectors are untouched. Seven
modules currently expose vector semantics — `colorbalance` (`lift`/
`gamma`/`gain`, with mode-gated aliases `offset`/`power`/`slope` writable
instead under the module's default `SLOPE_OFFSET_POWER` mode),
`channelmixerrgb` (`red`/`green`/`blue`/`saturation`/`lightness`/`grey`
mixing rows), `rgblevels` (`levels.linked` when `autoscale` is linked,
`levels.red`/`levels.green`/`levels.blue` when it is independent),
`borders` (`color`/`frame_color`), `watermark` (`color`), `negadoctor`
(`dmin`/`wb_high`/`wb_low`, since milestone 6), and `colorharmonizer`
(`custom_hue` — writable only when `rule` is `DT_COLORHARMONIZER_CUSTOM`,
a rule switch in the same call counts — and `node_saturation`, since
milestone 6) — and
`get_module_schema`'s `semantic_fields` (`"class": "vector"`) is the
authoritative source of each module's vector IDs, component count, and
per-component ranges. This requires a darktable that advertises the
`vector_params` hello capability; against an older darktable the tool
refuses client-side with an upgrade message instead of silently dropping
the vector half of a patch. `curves`, `vectors`, `bands`, and
`quantities` (below) may
be given together in the same call; a semantic ID given in more than one
raises a client-side error before anything is sent.

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

`set_module_params` also edits semantic band parameters (milestone 5)
through its optional `bands` argument: semantic IDs mapped to
`{"y": [samples], "x"?: [positions]}`. `y` replaces the whole named band
set (exactly the schema's `count` samples, each within its `y_range`);
unlisted bands are untouched. Four modules currently expose band
semantics — `atrous` (`bands.luma`/`bands.chroma`/`bands.sharpness`/
`bands.luma_threshold`/`bands.chroma_threshold`, six samples each),
`denoiseprofile` (`bands.all`/`bands.red`/`bands.green`/`bands.blue`/
`bands.y0`/`bands.u0v0`, seven samples each), `rawdenoise`
(`bands.all`/`bands.red`/`bands.green`/`bands.blue`, five samples each),
and `lowlight` (`bands.transition`, six samples) — and
`get_module_schema`'s `semantic_fields` (`"class": "bands"`) is the
authoritative source of each module's band IDs, sample count, and x
policy. This requires a darktable that advertises the `band_params` hello
capability; the tool refuses client-side with an upgrade message
otherwise.

`x` is accepted only on bands whose schema says `x_policy: "interior"`
(`atrous`, `lowlight`): endpoints must equal the stored endpoints, the
positions must be strictly ascending, and adjacent gaps must be at least
the schema's `min_gap`. On a `"fixed"`-policy module (`denoiseprofile`,
`rawdenoise`) sending `x` is an `unsupported_field` error. An atrous
example — a mid-frequency luma contrast boost that also shifts the
interior band positions:

```json
{
  "module": "atrous",
  "values": {},
  "bands": {
    "bands.luma": {
      "y": [0.5, 0.6, 0.7, 0.6, 0.5, 0.5],
      "x": [0.0, 0.15, 0.4, 0.6, 0.85, 1.0]
    }
  }
}
```

Note the twin mirroring side effect: `bands.luma` shares its x positions
with `bands.luma_threshold` (the schema's `x_shared_with` link), so the x
write above also moves the threshold channel's band positions — y values
stay independent. A denoiseprofile y-only example, softening chroma
denoising in the coarsest bands:

```json
{
  "module": "denoiseprofile",
  "values": {},
  "bands": {
    "bands.u0v0": { "y": [0.5, 0.5, 0.5, 0.5, 0.4, 0.3, 0.2] }
  }
}
```

`set_module_params` also edits semantic quantity parameters (milestone 6)
through its optional `quantities` argument: semantic IDs mapped to an
object of component values. One module currently exposes a quantity —
`temperature` (`wb.temperature`, a Kelvin + tint pair converted to and
from the stored RGB multipliers by the module itself) — and
`get_module_schema`'s `semantic_fields` (`"class": "quantity"`) is the
authoritative source of the component names, units, and ranges. Every
component must be sent — the pair is written atomically because the
conversion is joint; to change only the temperature, read the current
pair first and send the current tint back. This requires a darktable
that advertises the `quantity_params` hello capability; the tool refuses
client-side with an upgrade message otherwise. Setting daylight white
balance:

```json
{
  "module": "temperature",
  "values": {},
  "quantities": {
    "wb.temperature": { "temperature": 5500.0, "tint": 1.0 }
  }
}
```

Two things to know when using it:

* **Conflict with the raw coefficients.** Uniquely among semantic
  parameters, `temperature`'s native `red`/`green`/`blue`/`various`
  multiplier scalars stay writable alongside `wb.temperature` (they are
  a legitimate expert surface — copying coefficients between images,
  scripted pipelines). A single call that writes one of those scalars in
  `values` *and* sends `wb.temperature` in `quantities` is rejected
  atomically — the two would fight over the same storage. Pick one
  surface per call.
* **Readback is lossy.** `wb.temperature` is derived: reading it back
  runs the reverse conversion, so a written 5500 K reads back as
  approximately 5500 (in practice well under 1 K off), not exactly.
  Compare with tolerance; the stored multipliers, not the Kelvin/tint
  projection, are the authoritative state.

### Blend settings

`set_module_params` also edits a module instance's blend settings (mask
Tier 1) through its optional `blend` argument — opacity, blend mode,
blending colorspace, mask refinement controls, and the off/uniform mask
mode. Halving a module's overall effect:

```json
{
  "module": "exposure",
  "values": {},
  "blend": { "mask_mode": "uniform", "opacity": 50 }
}
```

This requires a darktable that advertises the `blend_params` hello
capability; the tool refuses client-side with an upgrade message
otherwise. WARNING: writing `colorspace` deterministically resets
`mode`, `reverse`, `fulcrum`, and any parametric-mask thresholds to the
new space's defaults — send replacement values in the same call if you
want them. The response's `blend` member reads back the complete
post-commit blend state.

### Parametric masks

With the `parametric_mask_params` capability (which implies `blend_params`),
the `blend` argument also reads and writes **parametric** ("conditional")
masks: per-channel trapezoid ramps over the module's input/output values,
with polarity, per-slot boost, and the mask-combine setting. Channels are
named `<name>_in`/`<name>_out` and depend on the effective blend
colorspace — Lab (`L a b C h`), RGB display (`g R G B H S l`), or RGB scene
(`g R G B Jz Cz hz`). RAW/NONE have no parametric support.

Selecting the **bright sky** on an exposure instance via scene luminance —
markers ramp the mask to full effect above ~0.65, so no inversion is
needed. `colorspace` is set explicitly in the same call (it applies first
and resets the blendif block, so send it before the thresholds):

```json
{
  "module": "exposure",
  "values": {},
  "blend": {
    "colorspace": "DEVELOP_BLEND_CS_RGB_SCENE",
    "mask_mode": "parametric",
    "combine": "exclusive",
    "parametric": {
      "Jz_in": { "markers": [0.55, 0.65, 1.0, 1.0] }
    }
  }
}
```

Each channel entry replaces that slot entirely (`markers` required,
`inverted` and `boost` optional); `null` resets a slot. A channel is
enabled only when its markers are not the full span `[0,0,1,1]`.

WARNING — **boost does not rescale markers.** Unlike the GUI, changing a
channel's `boost` on the wire never shifts its markers; if you want
GUI-equivalent thresholds you must rescale the markers yourself.

WARNING — **`combine` flips effective polarity.** An inclusive `combine`
XORs every channel's effective inversion (`effective_inverted = inverted
XOR (combine is inclusive)`), which flips what the mask selects. Changing
`combine` and an explicit `inverted` in the same patch is refused unless
you also pass `"allow_inverted_combine": true` to confirm.

`drawn ↔ drawn+parametric` transitions are owned by this parametric surface;
the `mask_shapes` tools own drawn-form creation and attachment.

### Drawn masks

With the `mask_shapes` capability, create a circle on exposure, inspect its
mask, adjust it, then detach it. Use the returned shape ID and latest revision
at each placeholder:

```text
create_mask_shape
{"type":"circle","space":"preview","geometry":{"center":[0.5,0.5],"radius":0.2,"border":0.03},"attach":{"op":"exposure","instance":0}}

render_preview
{"max_px":512,"show_mask":{"op":"exposure","instance":0}}

update_mask_shape
{"id":<shape_id>,"space":"preview","geometry":{"center":[0.55,0.5],"radius":0.18,"border":0.03},"expected_revision":<revision>}

set_mask_attachment
{"op":"exposure","instance":0,"shape_id":<shape_id>,"attached":false,"expected_revision":<revision>}
```

Coordinates are preview-normalized; scalar radii and borders are fractions
of the shorter rendered-image edge. Points map exactly, but perspective
correction can make size and angle conversion `size_mapping: "approximate"`;
check that result before treating a radius or rotation as exact. Shapes are
shared objects: `update_mask_shape` changes every module using the form, and
its `affects_instances` result tells you how many. Gradient updates preserve
the GUI's existing linear or sigmoidal transition mode.

Manual GUI-guard test: open the image in darkroom, start dragging a circle,
issue a remote `delete_mask_shape` for it, and confirm that darktable does
not crash and the drag ends cleanly.

### Seeing the mask

With the `mask_render` capability, `render_preview` takes an optional
`show_mask` argument that renders a module's blend mask (grayscale, **white
= full effect**) instead of the image, at the same framing as an ordinary
preview:

```json
{ "max_px": 512, "show_mask": { "op": "exposure", "instance": 1 } }
```

For a `show_mask` request the tool returns **mixed content**: a JSON text
block with `mime_type`, dimensions, `revision`, and `mask_of`
(`{op, instance, mask_mode}`), immediately followed by the native JPEG
image block. Use it to verify every parametric threshold you set — the mask
is rendered even when the target module is disabled, and `off`/`uniform`
masks render solid white.

## Setup

Requires Python >= 3.10. One command, from any directory (bash or zsh):

```sh
. tools/mcp/env.sh
```

Sourcing `env.sh` is idempotent and does all of the below on first use,
then just activates on subsequent uses:

* creates `tools/mcp/.venv` and installs this package with its `[dev]`
  extras (repairing a half-made venv if the install was interrupted);
* activates the venv in your current shell;
* exports the variables below;
* defines `dt-mcp-darktable`, a helper that launches darktable with remote
  control enabled in a scratch config dir (see below).

The exported set is the one [the Ubuntu setup
guide](docs/ubuntu-mcp-client-setup.md) otherwise asks you to redefine in
every new terminal:

| Variable | Value |
| --- | --- |
| `REPO` | repository root of the checkout you sourced from |
| `DARKTABLE_BIN` | in-tree GUI build: `build/bin/darktable`, else `build-mcp/bin/darktable` |
| `MCP_BIN` | `$REPO/tools/mcp/.venv/bin/darktable-mcp`, the sidecar launcher |
| `DT_CONFIG` | persistent dev profile, default `~/.config/darktable-mcp-dev` |
| `DT_CACHE` | its cache, default `~/.cache/darktable-mcp-dev` |

Each keeps a value you exported before sourcing. No directories are
created for `DT_CONFIG`/`DT_CACHE` — the guide's `mkdir -p` still does
that.

None of these is read by darktable or the sidecar; they are shell
variables you interpolate into flags (`--configdir`, `--library`,
`--config-dir`). The exception is `DARKTABLE_BIN`, which the integration
harness does read from the environment. `DT_CONFIG` in particular is
consulted by nothing: whatever you pass to darktable's `--configdir`
must be passed to the sidecar's `--config-dir` too, since discovery
matches on that directory. The sidecar does consult the environment
when you pass no `--config-dir` at all, but it reads
`$XDG_CONFIG_HOME/darktable` (`default_darktable_config_dir()` in
`discovery.py`) -- not the dev profile this guide sets up, and a path
a snap-confined terminal silently redirects.

<details>
<summary>Manual equivalent, if you prefer explicit steps</summary>

```sh
cd tools/mcp
python3 -m venv .venv
. .venv/bin/activate
pip install -e '.[dev]'
```

</details>

## Agent skill

Connecting the MCP gives an agent 17 tools; it does not teach it to use
them well. `skills/darktable-editing/` is an [Agent
Skill](https://agentskills.io/specification) that supplies the missing
half: the orient/inspect/change/verify loop, the rule that
`get_module_schema` is read before any first write, revision
compare-and-swap discipline, a natural-language intent → module table,
the four semantic parameter classes, the mask surfaces, and error
recovery. It is plain markdown — usable by any host implementing the
Agent Skills spec, not just Claude Code.

Install it for yourself (available in every project — the usual choice,
since you edit photos from wherever, not from this repo):

```sh
mkdir -p ~/.claude/skills
ln -s "$PWD/tools/mcp/skills/darktable-editing" ~/.claude/skills/
```

Run that from the repository root. A symlink means `git pull` keeps the
skill current; copy the directory instead if you would rather pin it.

Or install it for one project only, so a photo-editing workspace picks
it up and nothing else does:

```sh
mkdir -p /path/to/project/.claude/skills
ln -s "$PWD/tools/mcp/skills/darktable-editing" /path/to/project/.claude/skills/
```

Verify with `/skills` (or ask the agent to list its skills) — you should
see `darktable-editing`. It loads on its own when you ask for a photo
edit; the four files under `references/` are pulled in only when that
part of the surface is reached, so an ordinary exposure tweak does not
pay for the mask documentation.

Nothing about the MCP requires the skill, and nothing about the skill
requires you to install it from here — it reads the live
`get_module_schema` for anything version-specific, so it degrades to
"slightly out of date prose" rather than breaking when darktable moves.

## Running the tests

The unit tests are self-contained: they run a fake framed-JSON server
in-process (plain `asyncio` streams on loopback) and never require a real
darktable instance.

```sh
. tools/mcp/env.sh
pytest tools/mcp          # or: cd tools/mcp && pytest
```

The live end-to-end suite is marked `integration` and deselected by
default: it launches a real darktable GUI under a display server (Xvfb, or
your own `$DISPLAY`) and drives it over the wire. It needs a GUI build of
darktable — `env.sh` exports `DARKTABLE_BIN` for you when `build/bin/`
has one; otherwise the harness falls back to `build/bin/darktable` and
then `PATH`, and skips loudly if none is found:

```sh
. tools/mcp/env.sh
pytest tools/mcp -m integration
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
   (so it doesn't touch your real profile). With `env.sh` sourced this is
   one command:

   ```sh
   dt-mcp-darktable /tmp/dt-mcp-check   # defaults to /tmp/dt-mcp-scratch
   ```

   which is shorthand for:

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
`reset_module`, `create_module_instance`, `undo`, `create_mask_shape`,
`update_mask_shape`, `delete_mask_shape`, and `set_mask_attachment`) this
matters more, since
a blind retry would risk double-applying an edit -- which is exactly why
those methods take an `expected_revision` for compare-and-swap.
