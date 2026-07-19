# darktable MCP — mask support: tiered design candidates

Date: 2026-07-19
Status: candidate design sketches (no milestone scheduled; written from the
mask-support challenge research after milestone 6)
Companions: `2026-07-16-darktable-mcp-supported-operations.md` (current tier
state; masks recorded as out of scope), `2026-07-05-darktable-mcp-protocol-reference.md`
(the wire contract these sketches extend),
`2026-07-18-darktable-mcp-picker-and-sampling-candidates.md` (the
`sample_region` coordinate work Tier 3 depends on).

## Purpose

"Mask support" is three technically distinct surfaces in sharply increasing
order of difficulty. This document sketches a possible design for each so
the work can be scheduled as separate milestones behind separate
capabilities. Nothing here is committed; when a tier is picked up it goes
through brainstorming to a real design doc, and these sketches are the
starting material.

Source-of-truth facts the sketches rest on (verified 2026-07-19):

- Blend state lives in `dt_develop_blend_params_t` (`src/develop/blend.h:180`),
  a versioned struct (`DEVELOP_BLEND_VERSION` = 14) **outside**
  `module->params` and outside introspection.
- Blend edits commit through the ordinary `dt_dev_add_history_item`; drawn-mask
  edits commit through `dt_dev_add_masks_history_item` (routed via the
  `mask_manager` module). Both raise `DEVELOP_HISTORY_CHANGE`, so the
  revision tracker counts them, and the remote `undo` engine already
  filters on `DT_UNDO_DEVELOP`, which includes `DT_UNDO_MASK` — the
  history/undo/revision plumbing needs no new design.
- Drawn shapes are stored normalized to the **raw input image**
  (`dt_dev_distort_backtransform(screen)/iwidth`, `src/develop/masks/circle.c:281`)
  — the same coordinate back-transform problem as `sample_region`.

---

## Tier 1 — blend settings (`blend_params` capability)

**Goal.** Read and write the per-instance blend controls that need no mask:
opacity, blend mode, blend colorspace, fulcrum, feathering, blur, contrast,
brightness, details threshold. Highest conversational value per unit work:
"dial this module back to 50%" is `opacity`.

**Wire shape.** Extend the existing methods rather than adding new ones, so
one call stays one history item (an agent should be able to set exposure
−1 EV *and* opacity 50% atomically):

- `get_module_schema` gains a top-level `"blend"` object for modules with
  `IOP_FLAGS_SUPPORTS_BLENDING` (absent otherwise). Hand-written schema —
  introspection does not cover blend params — with the same field
  vocabulary as scalar params (`type`, `range`, `values`, `writable`).
- `get_module_params` gains `"blend": { ... }` with current values.
- `set_module_params` gains an optional `"blend": { ... }` patch object,
  validated and applied in the same transaction as `values`/semantic
  patches, still exactly one history item.

**Fields in v1** (wire name → storage):

| wire | storage | notes |
|---|---|---|
| `mask_mode` | `mask_mode` | v1 accepts only `"off"` / `"uniform"` (`DEVELOP_MASK_DISABLED` / `ENABLED`); modes involving drawn/parametric/raster masks are reported read-only until their tiers exist |
| `colorspace` | `blend_cst` | enum by name via `dt_develop_blend_colorspace_names`; writing it re-validates `mode` against the new space |
| `mode` | `blend_mode & DEVELOP_BLEND_MODE_MASK` | enum by name via `dt_develop_blend_mode_names`; obsolete/deprecated modes readable, not writable |
| `reverse` | `blend_mode & DEVELOP_BLEND_REVERSE` | the flag bit surfaced as a boolean, never as a raw ORed int |
| `fulcrum` | `blend_parameter` | float |
| `opacity` | `opacity` | stored 0–100 |
| `feathering_radius`, `feathering_guide`, `blur_radius`, `contrast`, `brightness`, `details` | same names | `feathering_guide` enum by name |

Not exposed in v1: `mask_combine` (meaningful only with drawn/parametric
masks — Tier 2/3), `blendif*` (Tier 2), `mask_id` and the `raster_mask_*`
trio (Tier 3 / out of scope), `reserved`, `feather_version`.

**Engine work.** A new `remote_blend.c` holding a static field table
(name, offset, type, range, enum tuple) — a hand-maintained
mini-introspection over the struct, pinned to `DEVELOP_BLEND_VERSION` with
a compile-time `sizeof` check so a struct change breaks the build, not the
wire. Patch application mirrors the scalar path: copy `blend_params` to a
temp block, validate the whole patch, commit via
`dt_iop_commit_blend_params` + the existing single-history-item sequence.

**Known hard part.** The mode × colorspace validity matrix has no exported
table; the GUI derives it in `src/develop/blend_gui.c` combobox population.
Either extract that into a shared table both consumers use, or accept any
`dt_develop_blend_mode_names` entry and let the pipeline clamp — the former
is right; the latter silently produces no-op edits.

**Errors.** Reuse the existing vocabulary: unknown blend field →
`unsupported_field`; out-of-range/invalid enum → `invalid_value` with the
established `{parameter, constraint}` details; `"blend"` sent for a module
without blending → `unsupported_field` naming `blend`.

**Testing.** Unit: schema/validation/readback against the fake dispatcher
(field table is pure). Integration: set opacity 50 on an enabled exposure
instance, assert the preview changes and history has one new item.

**Open questions for brainstorming.**
- Expose `mask_mode` at all in v1, or auto-manage it (`off` ↔ `uniform`)
  from whether blending fields are non-default?
- Sidecar argument name (`blend` vs `blending`) and whether the sidecar
  gates client-side on the capability exactly as `curves`/`vectors` do
  (it should).

---

## Tier 2 — parametric masks (`parametric_mask_params` capability)

**Goal.** Read and write blendif ramps: per-channel trapezoids over input
and output pixel values. Requires Tier 1 (it writes the same storage block
and needs `mask_mode` to gain `"parametric"` and `"uniform+parametric"`
values, plus `mask_combine` exposure).

**Storage model being wrapped.** `blendif_parameters[4 × 16]` — four
ascending markers per channel slot (trapezoid feet/shoulders) —
`blendif_boost_factors[16]` (log-scale display boosts), and the `blendif`
bitfield (per-channel enable + polarity bits). Channel slot *meaning*
depends on `blend_cst`: Lab uses L/a/b (+C/h in slots 8–13), RGB display
uses g/R/G/B (+H/S/l), RGB scene uses g/R/G/B (+Jz/Cz/hz); each has an
`_in` and `_out` half (`dt_develop_blendif_channels_t`).

**Wire shape.** Inside Tier 1's `"blend"` object:

```json
"blend": {
  "mask_mode": "parametric",
  "parametric": {
    "channels": {
      "Jz_in":  { "markers": [0.1, 0.2, 0.6, 0.7], "polarity": "normal" },
      "Cz_out": { "markers": [0.0, 0.0, 0.4, 0.5], "polarity": "inverted",
                  "boost": -2.0 }
    }
  }
}
```

- Channel names are the wire vocabulary; the engine owns all bitfield
  packing. The model never sees slot indices or raw bitmasks.
- The schema's `blend.parametric` section lists the valid channel names
  **for the module's current colorspace family**, each with its stored
  domain and the display-mapping formula as metadata (the GUI shows
  colorspace units; storage is normalized 0–1 — conversion authority stays
  with the client, documented, not silently applied, following the m6
  quantity precedent of never hiding the stored representation).
- A patch replaces the named channels' full 4-marker sets; unlisted
  channels are untouched; `"markers": null` disables a channel.
- Writing `parametric` requires `mask_mode` to include parametric — in the
  same call is enough, validated against *projected* params exactly like
  `writable_when` mode switches in the vector class.

**Validation.** Markers finite, within domain, ascending
(`m0 ≤ m1 ≤ m2 ≤ m3`); boost within the GUI's working range; channel name
not in the active colorspace family → `unsupported_field` naming the
channel; polarity string enum.

**Engine work.** Extends `remote_blend.c`; the packing/unpacking of
bitfield + marker arrays is pure and unit-testable. No new history or
threading design.

**Open questions for brainstorming.**
- Changing `colorspace` with parametric channels set: reject, or clear the
  ramps the way the GUI does? (Research the GUI's actual behavior first.)
- Expose the `details` threshold interaction (details slider is Tier 1,
  but its mask contribution belongs conceptually here).
- Is a `min_gap` between markers needed, or is `≤` ordering enough (GUI
  allows degenerate trapezoids = hard edges)?

---

## Tier 3 — drawn masks (`mask_shapes` capability)

**Goal.** List, create, edit, delete, and attach drawn shapes. The largest
milestone of the three; ship after `sample_region` so the coordinate
contract is designed once.

**Scope cut for v1.** Circle, ellipse, gradient only — small fixed-size
structs with clean invariants. Path and brush (variable-length Bézier
lists with per-point state, the crash-risk surface) and clone/object forms
deferred; `raster_mask_*` references stay out of scope entirely.

**Wire shape.** New methods, not `set_module_params` extensions — shapes
are per-image state in `dev->forms`, not per-module parameters:

- `list_mask_shapes` → all forms of the image: id, type, name, geometry,
  and which module groups reference them (with per-member combine state
  and opacity).
- `create_mask_shape { type, geometry }` → new form id (one masks history
  item).
- `update_mask_shape { id, geometry }`, `delete_mask_shape { id }`.
- `attach_mask { module, instance, shape_id, state?, opacity? }` /
  `detach_mask { ... }` — manages the module's group form, `mask_id`, and
  the drawn bit of `mask_mode` in one step; `state` is the combine mode
  (union/intersection/difference/exclusion) per group member.

Geometry per type mirrors the storage structs (`center[2]` + `radius` +
`border` for circle; ellipse adds `radius[2]`/`rotation`/`flags`; gradient
is `anchor`/`rotation`/`compression`/`steepness`/`curvature`).

**The coordinate decision (critical path).** Storage is raw-normalized;
the model reasons on the end-of-pipe preview. Recommendation: the wire
carries **preview-normalized coordinates** and the engine performs
`dt_dev_distort_backtransform` on write and the forward transform on read,
so the model never handles raw space. This is the same transform exposure
`sample_region` needs — design the coordinate contract (spaces, naming,
rounding, out-of-bounds policy) once, in that milestone, and inherit it
here. An explicit `"space": "preview"` field on the wire leaves room for a
raw-space escape hatch later.

**Engine work (the bulk of the milestone).**
- Extract a headless creation sequence from the GUI paths
  (`dt_masks_gui_form_save_creation` and per-shape handlers): create form,
  fill points, register in `dev->forms`, create/extend the module's group
  form, set `mask_id` + `mask_mode`, commit via
  `dt_dev_add_masks_history_item`. Same class of work as m6's temperature
  hooks but larger, and the first remote surface using the masks history
  path (revision tracking already covers it — see facts above).
- Strict geometry validation before anything touches `dev->forms`: finite,
  in-domain, radius/border positive, point-count caps. This is the first
  surface where a remote peer feeds variable geometry into pipeline code
  that assumes GUI-constructed invariants — treat it as adversarial input
  even on an authenticated loopback socket, and say so in the security
  notes of the eventual design doc.
- Form versioning: pin current per-shape versions; never write legacy
  forms.

**Testing.** Unit: geometry validation and group bookkeeping (forms logic
is exercisable headless — `reset_module` already manipulates forms in
tests). Integration: create circle → attach to exposure → preview-diff
asserts the effect is spatially confined (compare mean pixel change inside
vs outside the circle); delete → preview returns.

**Open questions for brainstorming.**
- Group semantics on the wire: expose darktable's group-form model
  directly (shapes reusable across modules) or a simplified
  one-group-per-module view? (The GUI supports shape reuse; hiding it
  risks corrupting existing edits' bookkeeping.)
- `update_mask_shape` on a form referenced by several modules edits all of
  them — surface that in the response, or require an explicit flag?
- Shape naming (`name[128]`): auto-generate like the GUI, accept a name,
  or both?
- Whether `mask_combine`'s inverted/inclusive bits (Tier 2 exposure)
  interact with per-member group states in a way the wire must model
  jointly.

---

## Cross-cutting notes

- **Capabilities**: `blend_params` (T1), `parametric_mask_params` (T2),
  `mask_shapes` (T3) — independent hello capabilities so tiers ship and
  gate separately; the sidecar refuses client-side per capability exactly
  as it does for `curves`/`vectors`/`bands`/`quantities`.
- **Ordering**: T1 next whenever scheduled (self-contained, high value);
  `sample_region` (picker doc) before T3; T2 any time after T1. T1 and T2
  reuse all existing history/undo/revision plumbing; T3 reuses it too but
  through the masks history entry point.
- **Struct-churn defense** (T1/T2): the hand-written field table is the
  single point of maintenance; compile-time `sizeof`/`offsetof` asserts
  against `dt_develop_blend_params_t` turn upstream struct changes into
  build failures instead of wire corruption.
- **Supported-operations doc impact**: T1 flips no module tiers (it adds a
  new orthogonal surface to every Tier-1 module with blending); the
  "blending out of scope" sentence gets replaced by a pointer to the
  capability matrix.
