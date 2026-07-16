# darktable MCP milestone 4 — semantic vector class (vectors, colors, levels)

**Date:** 2026-07-12
**Status:** implemented — revised 2026-07-12 after two design
reviews (all findings verified against the code; each delta below traces to
one).
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
| `borders` | `color[3]`, `frame_color[3]` | Tier 2 → Tier 1 (with the unused `*_text` strings appendix-denylisted, below) |
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
  `black < grey < white` with a minimum gap (rgblevels)

The subtype drives MCP-side schema descriptions and validation flavor, not
engine dispatch — exactly how the curve descriptor already carries
per-module flags (periodic, channel gating) without changing the curve
engine.

**Enum cleanup.** `dt_remote_parameter_class_t` (remote_parameters.h)
already reserves `DT_REMOTE_PARAMETER_LEVELS` from the three-class
taxonomy this design collapses. Milestone 4 **removes it** (and its case in
`dt_remote_semantic_patch_free`) alongside adding
`DT_REMOTE_PARAMETER_VECTOR`. The enum is internal and wire class names are
stable strings (the header says so explicitly), so removal has no wire
impact; leaving it would invite routing rgblevels through a dead id.
`DT_REMOTE_PARAMETER_SAMPLED_RESPONSE` stays reserved for milestone 5.

## Protocol and engine changes

The `semantic_values` envelope from milestone 2 was designed for this:
`dt_remote_semantic_patch_t` is `class_id` + union with `curve` as the only
arm. Milestone 4 adds:

1. **Union arm** — `dt_remote_vector_patch_t { char *name; GArray *values;
   /* double elements */ }` behind `DT_REMOTE_PARAMETER_VECTOR`.
   Components are carried as **doubles end-to-end until range validation**
   and narrowed to float only when written into the params blob. Validating
   in float domain would let a slightly out-of-range JSON number (e.g.
   2.00000001) round back into range (2.0f) and pass — the curve pipeline
   already parses through `_node_to_finite_double` into `double` points for
   this reason, and vectors follow it.
2. **Parser branch** (`remote_protocol.c`) — same hygiene as curves: reject
   unknown members, duplicate semantic IDs, non-numeric and non-finite
   JSON, and components beyond a global per-vector cap, before
   `remote_edit` is invoked. The parser checks shape only; it does not know
   descriptors. **The engine resolves semantic IDs against the registry and
   rejects component-count mismatches** — same division of labor as curves,
   where point-count limits are descriptor knowledge.
3. **Vector registry** — new `src/control/remote_vector_registry.c`
   patterned on `remote_curve_registry.c`: static adapters keyed by
   `(op, params_version)`. A vector descriptor carries:
   - semantic name (`"lift"`, `"levels.linked"`, `"color"`)
   - **a native introspection path** to the backing array field, plus an
     optional row index for 2-D arrays (`levels[row]`) — *not* a raw byte
     offset. Params structs are private to separately compiled IOP files,
     so core code cannot take `offsetof` on them; literal offsets would be
     ABI-fragile and unchecked. Introspection paths give the same
     properties curves already rely on: offset/type/capacity resolved and
     validated fail-closed at registry-validation time, and a field root
     for `represented_by` stamping (below).
   - per-component names and ranges (so colorbalance's component 0
     "factor" documents itself even though its stored range, 0.0–2.0,
     happens to match R/G/B)
   - subtype tag and, for `color`, a color-space hint
   - optional ordering constraint with spacing rule (levels; below)
   - optional `writable_when` predicates, `prepare_fields`, and
     `prepare`/`validate_completed` hooks — the same conditional machinery
     curve adapters already use
4. **Generalized dispatch** — `remote_edit.c` has **four** curve-specific
   seams, not three: schema listing, `represented_by` annotation
   (`_annotate_represented_by`, which walks descriptor native paths to
   stamp back-references onto primitive fields), value readback, and patch
   apply + readback verification. All four generalize to dispatch on
   `class_id` across both registries; the annotation seam is why
   descriptors must carry introspection paths. Curve behavior must be
   byte-for-byte unchanged; the milestone 2/3 test suites are the
   regression net.
5. **Tagged containers and serializers** — the patch union is tagged, but
   schemas and values are currently curve-typed:
   `dt_remote_module_schema_t.semantic_fields` holds
   `dt_remote_curve_schema_t`, `dt_remote_mutation_result_t.semantic_values`
   maps to `dt_remote_curve_value_t`, and `remote_protocol.c` serializes
   both through `_curve_schema_to_json`/`_curve_value_to_json`
   unconditionally. Milestone 4 introduces tagged semantic schema/value
   wrappers (class id + union, mirroring the patch envelope) with generic
   destructors; serializers switch on the tag. Aggregation order is
   deterministic — registry order within a class, curves before vectors —
   and **semantic IDs must be unique across classes** per module, enforced
   at registry validation.

No sidecar format changes: params blobs are untouched in layout; only the
write path grows.

## Wire and MCP contract

The wire already discriminates semantic rows with a `"class"` member
(`"curve"` today); vectors reuse it. Normative shapes:

- **Schema row** (`semantic_fields` in `get_module_schema`):

  ```json
  {
    "name": "lift", "class": "vector", "display_name": "lift",
    "subtype": "vector",              // "vector" | "color" | "levels"
    "color_space": "display_rgb",     // present only when subtype = "color"
    "readable": true, "writable": true,
    "writable_when": { "field": "mode", "op": "ne", "value": "SLOPE_OFFSET_POWER" },
    "components": [
      { "name": "factor", "minimum": 0.0, "maximum": 2.0 },
      { "name": "red",    "minimum": 0.0, "maximum": 2.0 },
      ...
    ],
    "ordering": {                     // present only when subtype = "levels"
      "rule": "strictly_increasing",
      "minimum_gap": 1.19e-7, "minimum_gap_comparison": "at_least"
    }
  }
  ```

  Subtype and color-space strings are stable wire vocabulary, like curve
  interpolation names.

- **Value** (params reads and mutation readback): `{"class": "vector",
  "active": ..., "effective": ..., "writable_now": ..., "values": [...]}` —
  the same status flags curve values carry.

- **Patch** (`semantic_values` request member): `{"<semantic id>":
  {"values": [...]}}` entries alongside curve entries.

- **Capability** — a new **`vector_params`** capability gates vector
  writes, exactly parallel to `curve_params` (which gates curve writes on
  top of the `semantic_params` read surface). The server never advertises
  `vector_params` without accepting vector entries in `semantic_values`.

- **MCP tool surface** — no new tools. `set_module_params` gains a
  `vectors` argument parallel to `curves`, translated client-side into
  `semantic_values` entries and gated on `vector_params`.
  `get_module_schema`/`get_module_params` need no signature change.

- **Docs** — the protocol reference
  (`2026-07-05-darktable-mcp-protocol-reference.md`) capability and
  `semantic_values` sections and the MCP server docstrings are updated in
  this milestone, not after it.

Native arrays stay read-only with `represented_by` pointing at the semantic
name(s) — one array may back several gated aliases (below), so
`represented_by` is a list, as it already is for curves sharing a root.

## Write semantics

- **Complete replacement per named vector**, same contract as curves. A
  patch supplies every component; partial component writes are a
  client-side read-modify-write. (Rationale: one validation story, no
  merge ambiguity; revisit only if real usage demands sparse writes.)
- Range validation clamps nothing — out-of-range components are rejected
  with the same error shape scalar writes use, and the check runs in
  double domain before narrowing (engine change 1).
- Readback verification after apply, exactly as curves do it.

## Module adapter specifics

Grounded in the current param structs:

- **colorbalance** — `lift[4]`, `gamma[4]`, `gain[4]`; channel order is
  `factor, red, green, blue` (`_colorbalance_channel_t`); all components
  `$MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0`. The `mode` enum changes what the
  arrays *mean*: the default is `SLOPE_OFFSET_POWER`, where the GUI labels
  the same storage offset/power/slope (lift↔offset, gamma↔power,
  gain↔slope per the GUI section headers); `LEGACY` is lift/gamma/gain in
  sRGB instead of ProPhoto. The adapter therefore exposes **six mode-gated
  aliases over the three arrays**: `lift`/`gamma`/`gain` writable when
  `mode != SLOPE_OFFSET_POWER`, `offset`/`power`/`slope` writable when
  `mode == SLOPE_OFFSET_POWER` — expressed with the existing
  `writable_when` EQ/NE predicates, `mode` in `prepare_fields` so a patch
  can switch mode and write the newly active names in one step (the
  colorzones precedent), and the readback `active` flag marking the
  dormant set. Gating makes alias conflicts structurally impossible: at
  most one name per array is writable under any composed mode. Component
  descriptions must state the stored-value convention — stored 1.0 is the
  identity, which the GUI displays as 0.0 (R/G/B, `set_offset(-1.0)`) or
  0% (factor) — and the per-mode color space, or clients will send
  GUI-style values.
- **channelmixerrgb** — six `float[4]` rows: `red`/`green`/`blue`
  (COLOR_MIN −2.0 … COLOR_MAX 2.0) and `saturation`/`lightness`/`grey`
  (−2.0 … 2.0). The GUI binds three components per row; the fourth
  element's usage must be confirmed during planning — the adapter exposes
  the GUI-visible components and preserves any reserved element verbatim
  (the basecurve reserved-channel precedent from milestone 3).
  **NaN guard:** `commit_params` divides the R/G/B rows by their sums when
  `normalize_R/G/B` is set, with no zero guard (the grey row has one), so
  an in-range row like `[1, -1, 0]` with its normalize flag on produces
  inf/NaN in the pipeline. The GUI permits this state, but a NaN render is
  never intentional: the adapter's composed `validate_completed` **rejects
  patches that leave a normalize-enabled row summing to exactly 0.0f after
  float narrowing** — the same comparison the module's own grey-row guard
  uses (`norm_grey == 0.f`); near-zero sums render extreme but finite
  results the GUI equally permits and are not rejected. Because
  validation runs on composed params, the check naturally covers row
  writes, normalize-flag scalar writes, and both in one patch. This is a
  deliberate, documented divergence from GUI parity.
- **rgblevels** — `levels[3][3]` plus `autoscale` and `preserve_colors`
  enums. Four stable names, all always advertised:
  `levels.linked` (row 0, writable when
  `autoscale == LINKED_CHANNELS`) and `levels.red`/`levels.green`/
  `levels.blue` (rows 0–2, writable when
  `autoscale == INDEPENDENT_CHANNELS`) — `levels.linked` and `levels.red`
  alias row 0 under mutually exclusive gates, the same aliasing pattern as
  colorbalance. **No reset hook:** the module's own `gui_changed` for
  `autoscale` only switches the displayed tab; stored rows are never
  reset, and linked mode's row-0 fan-out happens only in `commit_params`'
  pipeline-data copy. The adapter mirrors that — inactive rows are
  preserved verbatim. `autoscale` goes in `prepare_fields` so writability
  composes with a same-patch mode change (no destructive prepare logic).
  Ordering constraint per triple: `black < grey < white` with an
  **absolute minimum gap of `FLT_EPSILON`, `at_least` comparison**. The
  GUI has two inconsistent rules (0.05 during drags, `FLT_EPSILON` in
  picker paths), so it is not a single source of truth; `FLT_EPSILON` is
  chosen because (a) processing computes `1/(white − black)`, and an
  epsilon gap bounds the reciprocal at ~8.4e6 — finite — while strict
  ordering alone admits denormal gaps that overflow to inf, and (b) the
  module's pickers themselves create epsilon-gap states, so any larger
  minimum would break read-modify-write round-trips of module-created
  params.
- **borders / watermark** — `color[3]` (+ `frame_color[3]` for borders),
  subtype `color` with a display-referred-RGB color-space hint, components
  `["red","green","blue"]` in 0.0–1.0 (defaults 1.0 for border color, 0.0
  for frame and watermark color; introspection ranges to be confirmed
  during planning). Borders' `aspect_text`/`pos_h_text`/`pos_v_text`
  fields are marked UNUSED in source; they join the operations doc's
  `writable: false` appendix in this milestone so the Tier 1 claim is
  honest. Watermark's `filename`/`text`/`font` strings stay unsupported —
  hence near-Tier 1.

## Validation and safety

- Parser rejects malformed shapes before edit code runs (unknown names and
  members, duplicate IDs, non-finite values, oversized component lists);
  the engine rejects count mismatches against descriptors.
- Range checks run in double domain; nothing is clamped.
- Adapter-level `validate_completed` enforces composed rules — levels
  ordering/gaps, the channelmixerrgb normalize guard — after the patch is
  applied to temp params, matching the curve pipeline's
  composed-validation placement, with atomic rollback on failure.
- Unknown `(op, params_version)` combinations simply advertise no vectors
  — same silent-degrade behavior as the curve registry.

## Testing

Three gates, identical in kind to milestone 3, with per-descriptor depth —
19 semantic names ship in this milestone (six colorbalance aliases, six
channelmixerrgb rows, four rgblevels names, two borders colors, one
watermark color), and a round-trip that reads and writes through the same
wrong mapping proves nothing:

1. **C unit tests** — new `test_remote_vector` patterned on
   `test_remote_curve`: parser hygiene (including double-domain range
   rejection at the float boundary), per-descriptor component-order tests
   against independently constructed params blobs (not readback of our own
   writes), untouched-byte verification for everything a patch does not
   name (reserved fourth components, inactive rgblevels rows, non-aliased
   arrays), alias gating (colorbalance mode switches, rgblevels autoscale
   switches, same-patch mode+vector writes), ordering/gap rejection, the
   channelmixerrgb normalize guard with atomic rollback, and cross-class
   ID-uniqueness registry validation. Plus the full existing `ctest` suite
   as the curve regression net for the generalized dispatch and tagged
   containers.
2. **Python unit suite** — `tools/mcp` schema/wire-format tests for vector
   entries: schema rows (subtype/color-space/ordering members), the
   `vectors` tool argument translation, and `vector_params` capability
   gating.
3. **Live integration** — per-module round-trip gates (write → readback →
   render-diff) for the five adapters, plus undo/history/stale-revision
   coverage where the milestone 2/3 suites don't already provide it
   (verify during planning; reference, don't duplicate). Fixtures must
   make the written field effective: the watermark gate selects a
   color-consuming SVG (`simple-text.svg` — the default `darktable.svg`
   never references `$(WATERMARK_COLOR)`), and the borders gate sets
   `frame_size > 0` before diffing `frame_color`. A curve-regression run
   proves rgbcurve/tonecurve/colorzones/basecurve behavior is unchanged.

## Documentation

Updated in the same milestone (the milestone 3 convention):

- `2026-07-05-darktable-mcp-supported-operations.md` — tier tables and
  appendix rows for the five modules, including the new borders `*_text`
  appendix entry.
- `2026-07-05-darktable-mcp-protocol-reference.md` — `vector_params`
  capability, vector schema/value/patch shapes, subtype and color-space
  string vocabulary.
- MCP server docstrings — the `vectors` argument on `set_module_params`.

## Open items deferred to planning

- channelmixerrgb fourth-component semantics (exposed vs reserved).
- borders/watermark introspection ranges for color components.
- Whether existing milestone 2/3 suites already cover undo/history/
  stale-revision for semantic writes (reference vs add).
- Whether `remote_edit.c`'s generalized dispatch iterates registries via a
  small static table of class vtables or an explicit two-call sequence —
  implementation detail, decided in the plan.
