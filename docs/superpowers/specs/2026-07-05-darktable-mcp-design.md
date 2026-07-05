# darktable Conversational Companion MCP — Design

Date: 2026-07-05
Status: draft for review

## Goal

An MCP server that lets an LLM operate darktable conversationally as a **live
GUI companion**: darktable is open, the user talks ("brighten the shadows a
bit, and warm it up"), the model adjusts module controls, and the user watches
the darkroom update. The user's eye is the primary feedback loop; the model
can look at the image on demand.

**v1 scope: darkroom editing only.** No library operations (culling, tagging,
collections), no export management, no style authoring. Those can become v1.x
tools later without architectural change.

## Non-goals (v1)

- Headless / server operation (see Roadmap; the design keeps the door open)
- Editing blending, parametric masks, or drawn masks
- Autonomous multi-step editing without user confirmation
- Windows/macOS support (v1 transport is D-Bus, Linux session bus)

## Architecture

```
MCP client (Claude Code / Desktop)
   │  stdio (MCP protocol)
   ▼
darktable-mcp server (Python, official `mcp` SDK)
   │  Transport interface: call(op, args) → result
   ▼
DBusLuaTransport (v1)                      [future: LuaSocketTransport]
   │  org.darktable.service.Remote.Lua(string) → string
   ▼
running darktable (stock build, USE_LUA, Lua enabled)
   └─ dt.gui.action() → dt_action_process() → history item → pixelpipe
```

Code lives in the fork at `tools/mcp/` (Python package + Lua snippet
templates). No darktable C changes required for v1.

### Layering rule (hard boundary)

Tool definitions never touch D-Bus or compose Lua directly. They call the
`Transport` interface only. Each logical operation is **one** Lua snippet
(atomic under darktable's Lua lock — no read-modify-write across calls).
Swapping the transport (Lua socket script, structured D-Bus methods, headless
CLI) must not change any tool signature.

## Transport contract (v1: D-Bus `Lua`)

Distilled from the interface in `src/common/dbus.c`:

1. **JSON-only returns.** Every snippet ends with `return <json string>`.
   The D-Bus handler calls `luaL_checkstring` on the result; returning a
   table/boolean raises. The server wraps every snippet in a template that
   `pcall`s the body and JSON-encodes `{ok=..., data=...}` or
   `{ok=false, error=...}`.
2. **No binary through the pipe.** Images are written by Lua to a temp
   directory and returned as file paths; the server reads the files.
3. **Server-side timeout** per call (default 30 s; `look_at_image` 120 s).
   darktable's Lua queue has no cancel — a timeout marks the transport
   degraded and surfaces a clear error to the model.
4. **Poll, never cache.** No events on this interface, and the user edits
   concurrently. Every tool call re-reads the state it needs.
5. **Error mapping.** `org.darktable.Error.LuaError` and in-snippet `pcall`
   failures both map to a structured tool error: `{kind, message, hint}` with
   kinds `not_running`, `lua_disabled`, `no_image_open`, `unknown_control`,
   `value_out_of_range`, `timeout`, `internal`.
6. **Startup probe.** On server start: bus name present? `LuaEnabled`
   property true? darkroom open? Failures produce actionable messages
   ("start darktable", "enable Lua", "open an image in the darkroom").

## Tool surface (v1)

| Tool | Signature (informal) | Backing |
|---|---|---|
| `get_current_image` | → id, filename, dimensions, basic EXIF, darkroom open? | `dt.gui` / develop state |
| `list_modules` | → [{op, label, enabled, instances}] for the open image | action tree + iop list |
| `list_controls` | (module) → [{path, label, type, range, default, current, enum_options}] | action tree + introspection metadata |
| `set_control` | (module, control, value) → new value | `dt.gui.action(...)` with the *set* effect |
| `nudge_control` | (module, control, direction, size?) → new value | *up*/*down* effects |
| `enable_module` | (module, on) → state | action system |
| `reset_module` | (module) → ok | action system *reset* effect |
| `undo` | (steps=1) → ok | history stack |
| `get_history` | → recent history items | Lua develop/history API (never direct DB access — the running instance owns the database) |
| `look_at_image` | (max_px=1024) → MCP image content | Lua export to temp JPEG; server loads file |

Design notes:

- **Addressing.** v1 controls are addressed by action path. Action labels
  derive from translatable `$DESCRIPTION` strings — v1 explicitly requires
  darktable running with an English locale (documented limitation; fixed
  properly in v2, which addresses by C field names).
- **`set_control` values are absolute** in the control's native unit (EV,
  %, °). Values outside the hard range are clamped and the clamped value is
  returned, with a note — never a silent success at a different value.
- **`list_controls` is the model's vocabulary.** Output is compact and
  cached per (module, darktable session) — metadata is static per build even
  though *values* are not (values always re-read).
- **`look_at_image`** exports a preview of the darkroom image (longest side
  `max_px`) through the normal export path with current history applied, to
  the server's temp dir, returned as MCP image content. This is the hybrid
  perception loop: the model calls it when it wants to judge results or
  analyze the image before proposing edits.
- Every mutating tool returns the resulting state (value, enabled flag), so
  the model can verify without a second call.

## Safety model

- All edits flow through the action system → every change is a history item
  → **everything the model does is undoable in one place**, by user (Ctrl+Z)
  or model (`undo` tool).
- No destructive tools in v1: no delete, no overwrite-export, no file moves,
  no database writes outside history.
- No raw `run_lua(code)` tool. The Lua sent over the wire comes only from
  the server's own parameterized templates with escaped arguments.
- The D-Bus session bus is the trust boundary (same as the user's desktop
  session); the MCP layer narrows what the *model* can invoke.

## Error handling

- Transport errors (bus gone, timeout) → `not_running`/`timeout` with a
  recovery hint; server keeps running and re-probes on next call.
- `unknown_control` includes fuzzy-match suggestions from `list_controls`
  (the model will guess names; help it).
- Concurrent user edits are not conflicts: last write wins, state is
  re-read per call, history preserves both.
- If no image is open in the darkroom, all editing tools fail fast with
  `no_image_open`.

## Testing

- **Unit:** Lua snippet templating (escaping, JSON envelope), error mapping,
  clamping logic — pure Python, no darktable.
- **Integration (developer machine / CI with Xvfb):** launch darktable with
  `--configdir` pointing at a throwaway config + a test image; exercise every
  tool via the real D-Bus path; assert on returned state and on history
  length. Xvfb keeps CI headless — same code path as a visible session.
- **Golden conversation check (manual):** scripted sequence
  (get image → list controls → set exposure → look at image → undo) run
  before releases.

## Roadmap

**v1 — stock darktable, action-based (this spec).** Zero fork changes.
Proves the conversational loop. Limitation: only widget-exposed controls,
widget quantization applies, English-locale addressing.

**v2 — fork: introspection param engine.** Small C addition exposing
per-field param access built on the existing generated `get_f`/`get_p`
(`src/iop/iop_api.h:319-320`) and the preset-apply precedent
(`src/gui/presets.c`):

- setter: `(op, instance, {field: value})` → validate/clamp via
  introspection, write params, **one** history item per batch; GUI syncs via
  the standard external-change path
- schema dump: JSON of every module's fields (name, type, range, default,
  enum options) — replaces the action-tree vocabulary; locale-independent,
  auto-synced with the build; non-scalar fields marked opaque
- Lua binding `darktable.develop.set_params/get_params` (~150 lines,
  follows existing binding patterns; the fork's `src/lua/ai.c` is the
  precedent)
- MCP `set_control` keeps its signature; only the backing snippet changes.
  Action-based path remains as fallback on stock builds.
- Out of scope in v2: blending params (`blendop_data`, separate versioned
  struct), drawn masks, coordinate-blob params (retouch, liquify).

**v3 — headless (optional).** Same engine behind `darktable-cli --set
op.field=value` (inject synthetic history items the way `--style` does), or
run the full app under Xvfb with the v1/v2 transport unchanged. Decide only
if a server use case materializes.

## Open questions (deferred, not blocking)

- Multi-instance policy for v2 (edit instance 0 vs create new instance)
- Whether `look_at_image` should also return a luminance histogram summary
  (cheap to add, helps the model judge exposure without pixel-peeping)
- Distribution: ship `tools/mcp/` as pip package vs in-fork only
