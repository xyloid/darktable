# darktable MCP milestone 4 — semantic vector class (vectors, colors, levels)

**Date:** 2026-07-12
**Status:** approved design, pre-plan
**Builds on:** milestone 2 (`rgbcurve` semantic curves) and milestone 3
(`tonecurve`, `colorzones`, `basecurve` adapters), both merged on
`worktree-mcp-remote-edit` at `52b892a1cf`.

## Goal

Add one new semantic parameter class — the **named fixed-length vector** —
and ship five module adapters that use it:

| module | promoted data | tier change |
|---|---|---|
| `colorbalance` | `lift`/`gamma`/`gain` (the module's core) | Tier 2 → Tier 1 |
| `channelmixerrgb` | `red`/`green`/`blue`/`saturation`/`lightness`/`grey` mixing rows | Tier 2 → Tier 1 |
| `rgblevels` | `levels[3][3]` black/grey/white triples | Tier 3 → Tier 1 |
| `borders` | `color[3]`, `frame_color[3]` | Tier 2 → Tier 1 |
| `watermark` | `color[3]` | Tier 2 → near-Tier 1 (strings stay unsupported) |

Everything else — banded responses (`atrous`, `rawdenoise`,
`denoiseprofile`, `lowlight`), picker-derived vectors (`negadoctor`),
sensor-level writes (`rawprepare`), `temperature` multipliers — stays out of
scope. Banded responses are the milestone 5 candidate.

## Why one class, not three

The parameter-class investigation taxonomized vector, color, and levels as
separate classes, but all three are "a few ordered floats with per-component
meaning." Modeling them as one class with descriptor-carried semantics gives
one union arm, one parser branch, and one registry instead of three of each.
Color and levels become **subtype tags** on the descriptor:

- `vector` — plain named components (colorbalance, channelmixerrgb)
- `color` — components are a color triple; descriptor carries a color-space
  hint (borders, watermark)
- `levels` — components are ordered handles; descriptor enforces
  `black < grey < white` (rgblevels)

The subtype drives MCP-side schema descriptions and validation flavor, not
engine dispatch — exactly how the curve descriptor already carries
per-module flags (periodic, channel gating) without changing the curve
engine.

## Protocol and engine changes

The `semantic_values` envelope from milestone 2 was designed for this:
`dt_remote_semantic_patch_t` is `class_id` + union with `curve` as the only
arm. Milestone 4 adds:

1. **Union arm** — `dt_remote_vector_patch_t { char *name; float *values;
   guint count; }` behind a new `DT_REMOTE_PARAMETER_VECTOR` class id.
2. **Parser branch** (`remote_protocol.c`) — same hygiene as curves: reject
   unknown members, duplicate semantic IDs, wrong component counts,
   non-numeric and non-finite JSON, before `remote_edit` is invoked.
3. **Vector registry** — new `src/control/remote_vector_registry.c`
   patterned on `remote_curve_registry.c`: one static adapter per module,
   keyed by `(op, params_version)`. A vector descriptor carries:
   - semantic name (`"lift"`, `"levels.red"`, `"color"`)
   - byte offset + component count into the raw params blob
   - per-component names and ranges (so colorbalance's component 0
     "factor" documents itself even though its stored range, 0.0–2.0,
     happens to match R/G/B)
   - subtype tag and, for `color`, a color-space hint
   - optional ordering constraint (levels: strictly increasing handles)
   - optional gating/prepare hooks (rgblevels mode handling, below)
4. **Generalized dispatch** — `remote_edit.c`'s three semantic seams
   (schema listing, value readback, patch apply + readback verification)
   currently call `dt_remote_curve_*` directly. They generalize to
   dispatch on `class_id` across both registries. Curve behavior must be
   byte-for-byte unchanged; the milestone 2/3 test suites are the
   regression net.

No sidecar format changes: params blobs are untouched in layout; only the
write path grows. No new MCP tools: `set_module_params` accepts vector
entries in the existing `semantic_values` argument, and `get_module_schema`
advertises them the same way curves are advertised (native arrays stay
read-only with `represented_by` pointing at the semantic name).

## Write semantics

- **Complete replacement per named vector**, same contract as curves. A
  patch supplies every component; partial component writes are a
  client-side read-modify-write. (Rationale: one validation story, no
  merge ambiguity; revisit only if real usage demands sparse writes.)
- Range validation clamps nothing — out-of-range components are rejected
  with the same error shape scalar writes use.
- Readback verification after apply, exactly as curves do it.

## Module adapter specifics

Grounded in the current param structs:

- **colorbalance** — `lift[4]`, `gamma[4]`, `gain[4]`; channel order is
  `factor, red, green, blue` (`_colorbalance_channel_t`); all components
  `$MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0`. Three semantic vectors named
  `lift`, `gamma`, `gain` with component names
  `["factor","red","green","blue"]`.
- **channelmixerrgb** — six `float[4]` rows: `red`/`green`/`blue`
  (COLOR_MIN −2.0 … COLOR_MAX 2.0) and `saturation`/`lightness`/`grey`
  (−2.0 … 2.0). The GUI binds three components per row; the fourth
  element's usage must be confirmed during planning — the adapter exposes
  the GUI-visible components and preserves any reserved element verbatim
  (the basecurve reserved-channel precedent from milestone 3).
- **rgblevels** — `levels[3][3]` plus `autoscale` enum
  (`LINKED_CHANNELS` default / `INDEPENDENT_CHANNELS`). Follows the
  colorzones precedent: in linked mode the single writable name is
  `levels.linked` (channel 0); in independent mode `levels.red`,
  `levels.green`, `levels.blue`. Ordering constraint
  `black < grey < white` per triple (exact minimum-gap rule, if any, to be
  read from the module's GUI clamps during planning). A prepare hook
  handles `autoscale` changes within the same patch, mirroring
  colorzones' channel-switch reset semantics (and like colorzones, it
  must not clobber scalar writes in the same patch).
- **borders / watermark** — `color[3]` (+ `frame_color[3]` for borders),
  subtype `color`, components `["red","green","blue"]` in 0.0–1.0
  display-referred RGB (defaults 1.0 for border color, 0.0 for frame and
  watermark color; introspection ranges to be confirmed during planning).

## Validation and safety

- Parser rejects malformed shapes before edit code runs (count mismatch,
  non-finite, unknown names).
- Adapter-level `validate_completed` enforces subtype rules (levels
  ordering) after the patch is composed, matching the curve pipeline's
  composed-validation placement.
- Unknown `(op, params_version)` combinations simply advertise no vectors
  — same silent-degrade behavior as the curve registry.

## Testing

Three gates, identical in kind to milestone 3:

1. **C unit tests** — new `test_remote_vector` patterned on
   `test_remote_curve`: parser hygiene, per-adapter round-trips, ordering
   rejection, mode-switch prepare behavior, reserved-component
   preservation. Plus the full existing `ctest` suite as the curve
   regression net for the generalized dispatch.
2. **Python unit suite** — `tools/mcp` schema/wire-format tests for vector
   entries in `semantic_values`.
3. **Live integration** — per-module round-trip gates (write → readback →
   render-diff) for the five adapters, plus a curve-regression run
   proving rgbcurve/tonecurve/colorzones/basecurve behavior is unchanged.

## Documentation

`docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md`
tier tables and appendix rows for the five modules are updated in the same
milestone (the milestone 3 convention).

## Open items deferred to planning

- channelmixerrgb fourth-component semantics (exposed vs reserved).
- rgblevels minimum-gap rule between handles, if the GUI enforces one.
- borders/watermark introspection ranges for color components.
- Whether `remote_edit.c`'s generalized dispatch iterates registries via a
  small static table of class vtables or an explicit two-call sequence —
  implementation detail, decided in the plan.
