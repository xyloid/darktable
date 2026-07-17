# darktable MCP milestone 5 — sampled-response class (bands)

Date: 2026-07-16
Status: designed (not yet implemented)
Companions: `2026-07-05-darktable-mcp-design.md` (base protocol),
`2026-07-05-darktable-mcp-parameter-class-investigation.md` (class taxonomy,
step 3 of its suggested extension sequence),
`2026-07-12-darktable-mcp-milestone4-vector-class-design.md` (the structural
template this milestone follows),
`2026-07-16-darktable-mcp-supported-operations.md` (tier impact).

## Goal

Add a third semantic parameter class, the **sampled response** (wire name
`bands`): a named per-channel set of a fixed number of band samples. This
covers the parallel `x[channels][N]`/`y[channels][N]` band arrays that the
curve engine deliberately does not reach — fixed band counts, mostly fixed
x positions — in four modules:

- `atrous` (contrast equalizer) — Tier 3 → Tier 1
- `denoiseprofile` (denoise profiled) — Tier 2 → Tier 1
- `rawdenoise` (raw denoise) — Tier 2 → Tier 1
- `lowlight` (lowlight vision) — Tier 2 → Tier 1

After this milestone, every module remaining below Tier 1 requires external
state (files, pickers, acquisition, catalogs) or an object workflow — the
pure declarative-adapter territory is complete.

Two deferred debt items from the milestone-4 final review ride along:

1. **Composed-validation seam**: with three engines sharing the mutation
   dispatch, restructure `remote_edit.c` to partition entries by class once
   and give each engine only its own entries, with one shared final pass.
2. **ValueError/ToolError asymmetry**: normalize the sidecar's wire-error
   mapping (pre-existing since milestone 2).

## Why a new class, not curve reuse

The investigation doc kept sampled responses separate from control-point
curves on purpose, and the source confirms the invariants differ:

- Band count is **fixed per module** (atrous 6, denoiseprofile 7,
  rawdenoise 5, lowlight 6). Curves accept 2–20 points; bands accept
  exactly N.
- x positions are **fixed** for denoiseprofile and rawdenoise (evenly
  spaced; denoiseprofile's params migrations force them back to even
  spacing) and **partially writable** for atrous (interior nodes, endpoints
  pinned, twin-channel mirroring) and lowlight (interior nodes, endpoints
  pinned — the x-drag strip below the curve, `lowlight.c:684-686`). Curves
  own x per point unconditionally.
- Interpolation is not caller-selectable; each module hard-codes its own
  spline behavior.

Forcing these through the curve engine would weaken the curve contract's
invariants (variable point count, per-point x) exactly where these modules
need them rigid.

## Class model

A **band-set** is a named semantic parameter with:

- `count` — the fixed number of bands N (schema-advertised, immutable);
- `y` — N samples, each clamped to the module's y-range (all four modules:
  [0, 1], the GUI clamp);
- `x` — N positions in [0, 1], strictly ascending; read-only or partially
  writable according to the adapter's declared x policy.

x policies (schema-advertised per band-set):

- `fixed` — x is reported in values but rejected in patches
  (`unsupported_field`). denoiseprofile, rawdenoise, and — for the
  endpoint components — atrous and lowlight.
- `interior` — x[0] and x[N-1] are pinned; x[1..N-2] are writable with
  strictly ascending order and minimum gap **0.001** (the identical GUI
  clamps in `atrous.c:1406-1408` and `lowlight.c:684-686`), validated with
  the milestone-4 `at_least` gap vocabulary. atrous (all five sets, with
  twin sharing below) and lowlight (`bands.transition`, no twins).

**Twin-channel x sharing (atrous).** The GUI stores five channels but
displays three tabs; the luma tab draws `L` and `Lt` as top/bottom curves
sharing x, likewise `c`/`ct`; `s` stands alone. An x drag mirrors the new
position into the twin's x array (`atrous.c:1394-1408`). The adapter
reproduces this: a patch that writes x on `bands.luma` also writes the same
x to `bands.luma_threshold`'s native array (and vice versa), atomically in
the same params projection. The schema advertises the coupling via
`x_shared_with: "<semantic name>"` so callers know a single-band-set x
write has a documented side effect. A request that writes x on **both**
twins with different values is rejected (`invalid_value`) rather than
silently last-write-wins.

## Engine and registry

Following the milestone-4 template exactly:

- `src/control/remote_band.c` / `.h` — class engine: JSON parsing (doubles
  in stored space), intrinsic validation (count, finiteness, y-range,
  x policy, ordering/gap, twin-conflict), patch application onto a params
  blob, serialization for schema/values.
- `src/control/remote_band_registry.c` — per-module adapter table. Each
  adapter pins its params version **min==max** (atrous=2, rawdenoise=2,
  denoiseprofile=12, lowlight=1) so struct-layout drift fails closed.
- Native `x`/`y` array fields stay `writable: false` in the schema with
  `represented_by` pointing at the semantic names.

### Adapters — 16 semantic names

| module | semantic names | N | x policy |
|---|---|---|---|
| `atrous` | `bands.luma`, `bands.chroma`, `bands.sharpness`, `bands.luma_threshold`, `bands.chroma_threshold` | 6 | `interior`; luma↔luma_threshold and chroma↔chroma_threshold share x |
| `denoiseprofile` | `bands.all`, `bands.red`, `bands.green`, `bands.blue`, `bands.y0`, `bands.u0v0` | 7 | `fixed` |
| `rawdenoise` | `bands.all`, `bands.red`, `bands.green`, `bands.blue` | 5 | `fixed` |
| `lowlight` | `bands.transition` | 6 | `interior`; no twins |

Notes:

- denoiseprofile's `y0`/`u0v0` channels only affect rendering in the
  wavelets Y0U0V0 color mode, and `all`/`red`/`green`/`blue` in the RGB
  modes; all six are always writable (matching the GUI, which stores all
  channels regardless of the active mode). The noise-fit `a[3]`/`b[3]`
  arrays remain excluded (auto-set from the camera profile, already
  denylisted).
- `atrous.octaves` is auto-derived in `commit_params` from image
  dimensions (`atrous.c:684`); the stored field is ignored by the pipeline.
  It joins the internal-fields denylist (appendix of the supported-
  operations reference). `mix` stays an ordinary writable scalar.
- rawdenoise and lowlight band edits leave their sibling scalars
  (`threshold`, `blueness`) untouched; scalars and bands compose in one
  request like scalars and curves do today.
- **Amendment (post-Task-6 implementation review):** lowlight was
  originally classified `fixed`; the GUI in fact moves interior
  `transition_x` nodes via the drag strip below the curve
  (`lowlight_motion_notify`, `lowlight.c:684-686`) with pinned endpoints
  and the same 0.001 at-least neighbor clamp as atrous, and the shipped
  "night blooming" preset stores a non-default x (`lowlight.c:424`).
  Classifying it `fixed` would leave GUI-reachable states writable by hand
  but not remotely, against this doc's own GUI-parity principle — so
  `bands.transition` is `interior` (min gap 0.001, no twins).

## Wire and MCP contract

Mirrors milestone 4's shapes with `vector` → `bands` substitutions. The
normative JSON shapes will be copied into the protocol reference in the
documentation task, as milestone 4 did.

- **Capability**: `band_params` in the hello response. Never advertised
  without accepting band entries; band entries against a server without the
  capability fail with `unsupported_field`.
- **Mutation entry** (inside the combined mutation array): requires
  `"class": "bands"` (string member mandatory on every entry — parser rule,
  consistent with `curve`/`vector`), `"name"` (semantic name), and
  `"y"` (array of N numbers). Optional `"x"` (array of N numbers) only
  where the x policy is `interior`; when present it must carry all N
  components, and the pinned endpoints must equal the current stored
  endpoints after conversion to the native float type.
- **Whole-set replacement**: a band entry replaces the entire named
  band-set's y (and x when given); there are no partial/indexed writes,
  matching curve and vector semantics.
- **Schema**: each semantic name advertises `class: "bands"`, `count`,
  `y_range`, `x_policy` (`fixed`/`interior`), `min_gap` and
  `x_shared_with` where applicable, and current values in stored space
  (doubles).
- **Errors**: gated-off capability → `unsupported_field`; count mismatch,
  non-finite, y out of range, x ordering/gap violation, pinned-endpoint
  mismatch, twin x conflict → `invalid_value` with a `band_index` detail
  member when the failure is attributable to one band; adapter envelope
  failures (unknown module/version) → internal error. Same shape and
  atomicity rules as milestone 4: rejections are byte-atomic on the live
  blob (the projected temp blob is discarded by the caller).

## Composed-validation seam (ride-along 1)

Today `remote_edit.c` runs the curve engine then the vector engine over the
same temp blob, each passing the other class's entries through untouched.
A third engine makes the pass-through chain quadratic in review effort and
leaves multi-class modules (none exist yet, but atrous-style modules could
plausibly gain one) without a shared final validation point.

Restructure: `remote_edit.c` drives all seams from one class-ops dispatch
table (fixed order: curve, vector, bands), calling each engine
unconditionally against the same projected temp blob; each engine's
entry-point wrapper partitions out its own class's entries and keeps its
prepare gate, so scalar-only patches that name a prepare-field still reach
their engine. One shared final pass (param-version check, blob-size
invariants) runs after the table loop, before commit. Requirements:

- Byte-for-byte behavioral parity for existing curve and vector traffic —
  the gate is the existing `test_remote_curve` (69) and
  `test_remote_vector` (109) suites passing unchanged, plus the sidecar
  unit and integration suites.
- Unknown `class` strings keep failing with the same error they do today.
- Error precedence between classes in one request is deterministic
  (document the order: parse errors first, then per-engine intrinsic
  validation in dispatch order curve → vector → bands).

## Sidecar (tools/mcp)

- `set_module_params` gains a `bands` argument mirroring `curves` and
  `vectors`: a mapping of semantic name → `{y: [...], x: [...]}` (x
  optional), serialized into the combined mutation array with
  `class: "bands"`.
- **ValueError/ToolError cleanup (ride-along 2)**: normalize the
  pre-existing asymmetry where some argument-validation failures raise
  `ValueError` and others `ToolError`. All caller-input validation surfaces
  as `ToolError` with the wire error's code/message; `ValueError` remains
  only for programmer errors. Covered by the existing sidecar unit tests
  plus new cases for the changed paths.

## Validation and safety

Same defense-in-depth as milestone 4:

- All parsing in stored-space doubles; NaN/Inf rejected; array lengths
  checked against the adapter's count before any write; SIZE_MAX-guarded
  allocation arithmetic.
- Params versions pinned min==max; mismatch fails closed as internal error
  before any blob write.
- Undo/history: one request = one history item, uniform with scalars,
  curves, and vectors.
- GUI parity: applying a band patch then reading values back must match
  what the GUI produces for the same edit (fixtures; atrous x mirroring
  included).

## Testing

Mirroring milestone 4's structure:

- `test_remote_band` C unit suite: engine validation matrix (count,
  range, finiteness, x policies, gap, pinned endpoints, twin conflict,
  twin mirroring), per-adapter tests for all 16 names, serialization
  round-trips, version-pin failure.
- Cross-engine skip tests now cover all three pairings (curve/vector,
  curve/bands, vector/bands) in both directions, including semantic-name
  collisions across classes.
- Composed-dispatch regression: full existing curve and vector suites
  unchanged; new tests asserting mixed three-class requests apply
  atomically and reject atomically.
- Sidecar unit tests for the `bands` argument and the error-mapping
  cleanup; integration gates against live darktable (schema advertising,
  apply/read-back, GUI-parity fixtures, undo, error shapes).

## Documentation (same milestone, not after)

- Protocol reference: `band_params` capability section with normative JSON
  shapes and vocabulary (`fixed`/`interior`, `min_gap`, `x_shared_with`,
  `band_index`).
- Supported operations (2026-07-16 reference): m5 row in the milestone
  table; `atrous` → Tier 1 (since m5), `denoiseprofile`, `rawdenoise`,
  `lowlight` → Tier 1 (since m5); `atrous.octaves` appendix row.
- README + remote-control.md: `bands` argument with an atrous example
  (including x mirroring caveat) and a denoiseprofile example.
- This design doc's status line flips to implemented.

## Open items deferred to planning

- Exact JSON member spellings for the schema fields (`x_policy` vs
  `xPolicy` etc.) — the plan copies whatever the protocol reference
  normatively fixes; wire style follows milestone 4's snake_case.
- Whether the shared final pass in the composed seam can subsume the
  per-engine version pin checks or merely repeats them (either is
  acceptable; parity gates decide).
- denoiseprofile v12 struct layout details and any padding concerns for
  the pinned-version byte checks.
- Error precedence documentation location (protocol reference vs
  internals doc).
