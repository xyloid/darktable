# darktable MCP — drawn masks (mask Tier 3) low-level design

Date: 2026-07-19
Status: draft design (not yet planned; awaiting review; depends on Tier 1,
and shares its coordinate contract with the unscheduled `sample_region`
work — see "Coordinate contract")
Companions: `2026-07-19-darktable-mcp-blend-settings-design.md` (Tier 1),
`2026-07-19-darktable-mcp-parametric-masks-design.md` (Tier 2),
`2026-07-18-darktable-mcp-picker-and-sampling-candidates.md` (the
coordinate/back-transform problem this design inherits),
`2026-07-19-darktable-mcp-mask-support-design-candidates.md` (tier split).

This design goes as far as the current unknowns allow. Two areas are
deliberately specified as *policies with implementation-time pinning*
rather than final numbers: the per-shape geometry ranges (to be pinned
from the GUI interaction clamps during planning) and the scalar-size
coordinate conversion (specified as a concrete probe algorithm, flagged
as the highest-risk component). Everything else — wire methods, engine
sequences, group semantics, validation policy, history/undo behavior,
testing — is designed to plan-ready depth.

## Goal

List, create, edit, delete drawn mask shapes, and attach/detach them to
module instances with combine state and per-member opacity — behind a
`mask_shapes` hello capability, using new wire methods (shapes are
per-image state in `dev->forms`, not module parameters).

## Scope

**v1 shapes: circle, ellipse, gradient** — fixed-size structs with clean
invariants. Deferred: path and brush (variable-length Bézier lists with
per-point state — the adversarial-input surface), clone forms
(`DT_MASKS_CLONE`, retouch/spots territory), AI object forms, and raster
masks (`raster_mask_*`, `DEVELOP_COMBINE_MASKS_POS`).

## Verified source facts this design rests on

All verified 2026-07-19:

- **Creation is already headless-capable.**
  `dt_masks_gui_form_save_creation(dev, module, form, gui)`
  (`src/develop/masks/masks.c:336`) accepts `gui == NULL`: it assigns a
  unique id (`_check_id`), derives a unique name via the shape's
  `set_form_name`, appends to `dev->forms`, commits a masks history item,
  then — given a module — finds or creates the module's group form
  (`_group_create` sets `blend_params->mask_id`), appends a
  `dt_masks_point_group_t` member with
  `state = SHOW|USE` (`|UNION` when the group already has members),
  `opacity = conf "plugins/darkroom/masks/opacity"`, and commits a second
  masks history item. All GUI touches are behind `if(gui)`. **The engine
  calls this function directly; no re-derived sequence.**
- **Deletion is engine-ready.** `dt_masks_form_remove(module, grp, form)`
  (`masks.c:1793`) removes group membership, commits masks history,
  auto-deletes an emptied group, and handles full-form removal.
  `dt_remote_reset_module` already calls it headless today.
- **Storage coordinates.** Shape points are normalized to the raw input
  image: `center = dt_dev_distort_backtransform(screen_pts)/iwidth`
  (`masks/circle.c:281`); scalar sizes (radius, border) are normalized by
  the raw dimensions (legacy rescale uses `MIN(width,height)`,
  `masks.c:624`). The transform entry points are
  `dt_dev_distort_transform_plus` / `dt_dev_distort_backtransform_plus`
  (`develop.h:575,584`), operating on flat point arrays in pipe
  coordinates.
- **Group member model.** `dt_masks_point_group_t` = `{formid, parentid,
  state, opacity}`; states are `USE`, `SHOW`, `INVERSE`, and the combine
  ops `UNION`, `INTERSECTION`, `DIFFERENCE`, `EXCLUSION`, `SUM`
  (`masks.h:52`). Brush members default to `SUM`, others to `UNION`.
- **mask_mode.** "drawn mask" = `DEVELOP_MASK_ENABLED|DEVELOP_MASK_MASK`;
  "drawn & parametric" adds `CONDITIONAL` (`dt_develop_mask_mode_names`).
- **History/undo.** Masks history items route through
  `dt_dev_add_masks_history_item` → `_dev_add_history_item_ext(..., TRUE)`
  which snapshots params + blend params + forms and raises
  `DEVELOP_HISTORY_CHANGE`; the revision tracker counts each delivery,
  and remote `undo` (`DT_UNDO_DEVELOP` ⊇ `DT_UNDO_MASK`) already covers
  mask items.
- Shape geometry structs: circle `{center[2], radius, border}`; ellipse
  `{center[2], radius[2], rotation, border, flags}` with flags
  `EQUIDISTANT|PROPORTIONAL`; gradient `{anchor[2], rotation,
  compression, steepness, curvature, state}` (`masks.h:146-203`).
  Conf defaults: circle size/border 0.05, ellipse radii 0.05/0.03535.

## Coordinate contract

The wire speaks **preview-normalized coordinates**: `[0,1]²` over the
full rendered image exactly as `render_preview` frames it. The engine
converts both directions; the model never sees raw space numbers it has
to interpret (they are *reported* for debugging, see below).

- **Points (centers, anchors) — exact.** Write: preview-normalized →
  multiply by full-pipe processed dimensions →
  `dt_dev_distort_backtransform` → divide by raw dimensions → store.
  Read: the exact inverse via `dt_dev_distort_transform`. This is the
  same machinery every GUI shape handler uses; exactness is inherited.
- **Scalar sizes and angles — probe-based, documented as approximate
  under non-uniform distortion.** A stored radius is a length in raw
  space; under crop/rotate/perspective there is no single correct
  preview-space equivalent. v1 algorithm: back-transform a probe cross —
  the center plus 4 points at the requested preview-space radius (up,
  down, left, right); the stored radius is the mean raw-space distance
  from the transformed center to the 4 transformed probe points
  (normalized per storage convention). Angles (ellipse/gradient
  rotation): back-transform a short direction probe and take the raw
  angle delta. Reads use the forward equivalent. Under pure
  crop/flip/scale this is exact; under perspective it is a local mean —
  the response carries `"size_mapping": "exact" | "approximate"`
  computed by comparing the 4 probe distances (spread > 1% ⇒
  approximate).
- Every request and response geometry object carries `"space":
  "preview"`; an explicit field so a raw-space escape hatch can be added
  later without ambiguity. Responses additionally include
  `"raw_geometry"` — the stored values verbatim — for round-trip
  debugging and as the stable representation that survives geometry
  changes elsewhere in the pipe.
- **Shared contract note:** this is the same transform exposure
  `sample_region` needs. Whichever ships first implements the
  preview↔raw point mapping as a small shared engine helper
  (`remote_transform.c`); the other reuses it.

## Wire contract

### Capability

`hello` advertises `mask_shapes`. All six methods below require it; the
sidecar hides/refuses the tools without it.

### `list_mask_shapes` (read-only)

Response:

```json
{ "shapes": [
    { "id": 12, "type": "circle", "name": "circle #1",
      "space": "preview",
      "geometry": { "center": [0.62, 0.41], "radius": 0.11,
                    "border": 0.04, "size_mapping": "exact" },
      "raw_geometry": { "center": [0.598, 0.463], "radius": 0.093,
                        "border": 0.05 },
      "used_by": [
        { "op": "exposure", "instance": 1, "state": ["union"],
          "inverted": false, "opacity": 0.85 } ] } ],
  "revision": 41 }
```

- `used_by` is derived by walking every module's group form; `state`
  lists the combine op, `inverted` maps `DT_MASKS_STATE_INVERSE`,
  `opacity` is the group-member opacity (not the module's blend
  opacity).
- Group forms themselves are **not** listed as shapes; they are
  represented through `used_by`. (Decision 4 below.)
- Deferred-type forms present in the edit (paths, brushes, clones) are
  listed with their `id`, `type`, `name`, and `used_by` but **no
  geometry** and `"editable": false` — visible, attachable, not
  creatable/editable via this API.

### `create_mask_shape`

```json
{ "type": "circle",
  "space": "preview",
  "geometry": { "center": [0.62, 0.41], "radius": 0.10, "border": 0.03 },
  "name": null,
  "attach": { "op": "exposure", "instance": 1 },
  "expected_revision": 41 }
```

- `attach` is optional. With it, the engine passes the module to
  `dt_masks_gui_form_save_creation` (group + membership + `mask_id`
  handled by darktable) and then ORs
  `ENABLED|MASK` into `mask_mode` before the same call's final commit;
  without it, the form is created unattached (`module = NULL`), for
  later `attach_mask`.
- `name: null` → darktable's auto-numbering; a string requests that
  name (uniquified with a suffix if taken, actual name returned).
- Result: the new shape's `list_mask_shapes` entry + new revision.

Geometry members per type (all floats finite; ranges pinned at planning
from the GUI clamps, with the *policy*: positive sizes, borders ≥ 0,
centers/anchors permitted moderately outside [0,1] exactly as the GUI
allows placing shapes partly off-canvas, everything else rejected):

| type | members |
|---|---|
| `circle` | `center [x,y]`, `radius > 0`, `border ≥ 0` |
| `ellipse` | `center [x,y]`, `radius [a,b] > 0`, `rotation` (degrees), `border ≥ 0`, `border_mode`: `"equidistant"` \| `"proportional"` |
| `gradient` | `anchor [x,y]`, `rotation` (degrees), `compression > 0`, `steepness`, `curvature` |

### `update_mask_shape`

```json
{ "id": 12, "space": "preview",
  "geometry": { "center": [0.60, 0.45], "radius": 0.12, "border": 0.03 },
  "expected_revision": 42 }
```

- Full-geometry replacement of one shape (same members as create; type
  cannot change). Engine: back-transform, overwrite the point struct,
  one masks history item, reveal each referencing module? No — reveal
  the single module when exactly one references it, none otherwise.
- The response repeats `used_by` and carries
  `"affects_instances": <count>` — an update to a shared shape edits
  every module using it; the count makes that visible (decision 5).
- Renaming: optional `"name"` member.

### `delete_mask_shape`

```json
{ "id": 12, "expected_revision": 43 }
```

Engine: for each referencing module, `dt_masks_form_remove(module, grp,
form)` (which also drops emptied groups); if unreferenced, remove from
`dev->forms` with a masks history item. When a module's group empties,
the engine also clears `MASK` from its `mask_mode` (mirroring GUI
behavior when the last shape goes). Response: `removed_from` list of
affected instances + revision.

### `attach_mask` / `detach_mask`

```json
{ "op": "exposure", "instance": 1, "shape_id": 12,
  "state": "union", "inverted": false, "opacity": 0.8,
  "expected_revision": 44 }
```

- `attach_mask` **upserts**: creates the module's group + `mask_id` if
  missing (same `_group_create` path via a small exported helper — see
  Engine), appends or updates the membership (`state` → the
  `DT_MASKS_STATE_*` op bit plus `USE|SHOW`, `inverted` →
  `INVERSE`, `opacity` clamped to [0,1]), ORs `ENABLED|MASK` into
  `mask_mode`, one masks history item. Defaults: `union`, not inverted,
  conf opacity.
- `state` vocabulary: `"union" | "intersection" | "difference" |
  "exclusion"` (`"sum"` reserved for brush, refused in v1).
- `detach_mask` removes the membership via `dt_masks_form_remove(module,
  grp, form)` — the shape itself survives if unreferenced elsewhere
  (unlike `delete_mask_shape`), clearing `MASK` from `mask_mode` when
  the group empties.
- Attaching deferred-type shapes (an existing brush, say) is allowed —
  membership is type-agnostic — which lets an agent reuse hand-drawn
  brushes on other modules without being able to edit them.

### Errors

| condition | error |
|---|---|
| unknown shape id | `not_found`, `parameter: "id"` |
| `type` not in v1 set (create) or shape not editable (update) | `unsupported_field`, `parameter: "type"` |
| non-finite / out-of-policy geometry member | `invalid_value` with `{parameter, constraint}` |
| `state: "sum"` on non-brush | `invalid_value`, `constraint: "sum_is_brush_only"` |
| attach to module without blending/mask support (`IOP_FLAGS_NO_MASKS`, missing `SUPPORTS_BLENDING`) | `unsupported_field`, `parameter: "op"` |
| revision mismatch | `revision_conflict` (existing CAS semantics) |
| module ref not found | existing `not_found` vocabulary |

All six methods run on the GTK main thread via the existing dispatch;
all mutating ones take `expected_revision` CAS like every mutation.

## Engine design

### New file: `src/control/remote_masks.c` (+ `.h`)

- Per-type geometry (de)serializers: JSON ↔ `dt_masks_point_circle_t` /
  `_ellipse_t` / `_gradient_t` — pure, unit-testable.
- Validation: per-member finiteness + policy ranges **before** any
  `dt_masks_create` call; nothing invalid ever reaches `dev->forms`.
  Treat all geometry as adversarial input (first remote surface feeding
  variable structures into pipeline-adjacent code); cap: at most one
  shape per create call, name length < sizeof(form->name), reject
  unknown members.
- Form construction: `dt_masks_create(type)` + one malloc'd point struct
  + `g_list_append` — then hand off to
  `dt_masks_gui_form_save_creation(dev, module_or_NULL, form, NULL)`.
- `used_by` walk, state-bit ↔ string mapping, membership upsert.
- Coordinate conversion (`remote_transform.c` helper): point mapping via
  `dt_dev_distort_transform/backtransform` on the full pipe + the probe
  algorithm for sizes/angles. The full pipe must be in a usable state;
  the calls follow the same locking the GUI handlers rely on (they run
  on the GTK thread against `darktable.develop`, as all engine calls
  already do).

### One darktable-core change

`_group_create` (`masks.c:305`) is static; `attach_mask` on a module
with no existing group needs it. Export as
`dt_masks_group_create_for_module(dev, module)` (rename-and-export, GUI
path updated to call it — behavior-identical refactor, same pattern as
Tier 1's mode-table extraction). Everything else engine-side uses
already-public masks API.

### History, revision, undo

- Creation with attach commits **two** masks history items (darktable's
  own sequence: form, then group) plus the `mask_mode` OR folded into
  the second; the revision tracker counts each synchronous delivery and
  the mutation result reports the final revision — the
  `create_module_instance` precedent (also 2 items) applies verbatim,
  including the force-bump fallback.
- **Undo granularity wart (accepted, documented):** remote `undo`
  reverts one history item; undoing a `create_mask_shape`+attach takes
  two undo calls (the GUI has the same layered behavior). The reference
  documents this per method ("this call produces N history items").
  Batching into one undo record is explicitly out of v1 scope.
- `get_history` already lists mask items (they are ordinary history
  entries via the mask_manager/module); no change.

### Reveal behavior

Mutations reveal the affected module (Tier 0 behavior shipped 2026-07-18)
when exactly one module is affected; `list_mask_shapes` reveals nothing.

## Sidecar changes

Six new tools mapping 1:1 to the methods, gated on `mask_shapes`;
`space` defaults to `"preview"` client-side. Docstrings carry the two
model-facing warnings: shared-shape edits affect every user
(`affects_instances`), and size mapping may be approximate under
perspective correction (`size_mapping`). README gains a drawn-mask
walkthrough: create circle on exposure → verify via preview → adjust →
detach.

## Protocol reference impact

New top-level "Drawn masks (`mask_shapes`)" section: the coordinate
contract (including the probe algorithm and `size_mapping`), all six
method shapes, the state vocabulary, per-method history-item counts, the
deferred-type visibility rules, and the error table. Supported-operations
doc: the Tier 3 sentence updates; `mask_manager` stays unsupported as a
*module* (the methods replace it).

## Testing

Unit (`test_remote_masks.c`, headless with real modules via `dt_init`):
- geometry (de)serializers round-trip; every validation rejection row.
- create-unattached → form in `dev->forms` with unique name/id; create
  with attach → group exists, `mask_id` set, membership state/opacity
  correct, `mask_mode` has `ENABLED|MASK` (assert against a live
  exposure instance).
- attach upsert paths (no group / existing group / existing membership);
  detach empties group → group gone, `MASK` cleared; delete referenced
  shape → all memberships gone.
- state-bit mapping both directions incl. `INVERSE`; sum refusal.
- headless coordinate identity: with an identity pipe (no distortion),
  preview↔raw conversion is exact and `size_mapping == "exact"`.
- history-item counts per operation (revision tracker deltas).

Protocol: fixtures for all six methods, shared with `test_protocol.py`.

Integration:
- circle on exposure at a known position → preview-diff confined to that
  region (inside/outside mean-delta assertion — the spatial gate);
  update moves the region; detach makes the effect uniform; delete after
  detach leaves rendering unchanged.
- coordinate round-trip under distortion: enable `crop` + rotation,
  create at preview position P, `list_mask_shapes` returns P within
  tolerance (the back-transform gate — the highest-value test in the
  milestone).
- deferred-type listing: a fixture edit containing a brush lists it
  non-editable; attach succeeds.
- undo twice after create+attach restores the pre-call state.

## Decisions recorded (and what review should challenge)

1. **Reuse `dt_masks_gui_form_save_creation` headless** rather than
   re-deriving the sequence — it is already NULL-gui-safe; the one core
   export needed is `_group_create` (behavior-identical refactor).
2. **Preview-normalized wire coordinates with engine-side transform**;
   raw geometry reported alongside; explicit `space` field. Points
   exact, sizes/angles via the probe algorithm with honest
   `size_mapping` reporting — the alternative (raw-space wire) pushes an
   impossible task onto the model.
3. **v1 = circle/ellipse/gradient**; deferred types are visible and
   attachable but not creatable/editable — reuse without the
   variable-length attack/complexity surface.
4. **Groups are not wire objects**: darktable's group forms stay an
   engine-managed implementation detail surfaced as `used_by`/
   membership operations. Shape reuse across modules is fully supported;
   only the group *container* is hidden. Challenge: power users who
   nest groups in the GUI's mask manager will see flattened membership
   only.
5. **Shared-shape edits are global** with `affects_instances` in the
   response rather than a confirmation flag — the wire stays
   deterministic; the count keeps the agent informed.
6. **Undo granularity matches history items** (N calls to fully revert
   an N-item operation), documented per method; no batching in v1.
7. Per-member `opacity` and `inverted` live on `attach_mask`, not on
   the shape — mirroring storage (`dt_masks_point_group_t`), so the
   same shape can be inverted on one module and not another.

## Explicitly unresolved (to close during planning/brainstorming)

- Final numeric ranges per geometry member (pin from GUI interaction
  clamps; the validation *policy* above is fixed).
- Probe algorithm details: probe arm length for angle mapping, the
  exact/approximate spread threshold (1% is a placeholder).
- Whether `update_mask_shape` on a shape whose only user is a *disabled*
  module should still reveal that module.
- Interaction with `dt_masks_form_duplicate` (GUI shape duplication) —
  a possible cheap `duplicate_mask_shape` method, deferred unless a
  workflow needs it.
- Whether creation should honor the GUI's per-shape conf defaults for
  omitted optional members (`border` etc.) or require every member
  explicitly (current design: require explicitly; challenge welcome).
