# darktable MCP — drawn masks (mask Tier 3) low-level design

Date: 2026-07-19
Status: **planned** (see `docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md`; depends on Tier 1 / M-A and Tier 2 / M-B).
The two planning-prerequisite contracts are now resolved and written into
the plan as normative amendments: the pipe-freshness contract for
coordinate transforms (Amendment 1, shared verbatim with `sample_region`)
and the GUI-edit-session guard (Amendment 2 / decision 8). All items in
"Explicitly unresolved" below are closed — see the **Amendments
(planning)** section for the resolutions. The cross-tier `mask_mode`
transition appendix in the candidates doc is authoritative over this
document's per-operation mode effects.
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

- Legacy `dt_masks_form_remove(NULL, NULL, form)` scans only immediate
  members of module base groups and does not retain unlinked objects.
  Remote full shape deletion therefore uses
  `dt_masks_form_remove_shape_full`, which removes incoming references at
  every nesting depth and retires objects into `dev->allforms`.
- Remote creation uses `dt_masks_gui_form_save_creation_ext`. The first
  forced-new masks snapshot contains the unattached form and unchanged
  module state; the second contains the membership and the
  `ENABLED|MASK` mode addition. Both preserve `module->enabled`.
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
- **History/undo.** Remediated remote create, update, and full delete use
  forced-new masks snapshots through `dt_dev_add_new_masks_history_item`;
  creation reaches it through `dt_masks_gui_form_save_creation_ext`, and
  full delete records its module and global entries with it. Legacy and
  compatibility paths retain `dt_dev_add_masks_history_item`. Both route
  through the shared masks-history wrapper and remain covered by remote
  `undo` (`DT_UNDO_DEVELOP` ⊇ `DT_UNDO_HISTORY`). Each forced-new call
  bypasses edited-target suppression and places its signal-backed record
  in its own history undo group, so adjacent forced snapshots cannot
  time-coalesce; ordinary masks-history calls keep their existing merging.
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

- Nested `used_by` is flattened to one entry per referencing module.
  State, inversion, and opacity come from the target edge reached first by
  an ordered depth-first traversal from that module's base group: for each
  membership in list order, test the target and then recurse immediately
  before advancing to the next membership.
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

- `attach` is optional. The engine calls
  `dt_masks_gui_form_save_creation_ext` with explicit creation options:
  attached creation adds `ENABLED|MASK` only in the second forced-new
  snapshot, while unattached creation adds no mode bits. Both paths
  preserve `module->enabled`; group + membership + `mask_id` remain
  handled by darktable.
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

Engine: `dt_masks_form_remove_shape_full(dev, form, &references)` removes
incoming references at every nesting depth, retires the shape and any
newly unowned groups into `dev->allforms`, and clears `MASK` when a
module's base group empties. Response: `removed_from` list of affected
instances + revision.

### `attach_mask` / `detach_mask`

> **Superseded by Amendment 3:** these two are merged into a single
> `set_mask_attachment { …, attached: true|false }` method. The payload
> below is unchanged for the attach case (`attached: true`); `attached:
> false` detaches and reads none of `state`/`inverted`/`opacity`. Per
> Amendment 6, `state` is ignored when the shape lands as the group's
> first member.

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
  `dt_masks_gui_form_save_creation_ext(dev, module_or_NULL, form, NULL,
  &options)`.
- `used_by` walk, state-bit ↔ string mapping, membership upsert.
- Coordinate conversion (`remote_transform.c` helper): point mapping via
  `dt_dev_distort_transform/backtransform` on the full pipe + the probe
    algorithm for sizes/angles. Locking follows the GUI handlers (GTK
  thread against `darktable.develop`). **Freshness is a separate
  contract** (hard-challenge H3): locking prevents races, not staleness —
  a transform issued between a distortion mutation and pipe reprocess can
  be deterministically wrong with no redraw to self-correct. **Resolved as
  Amendment 1** (block-until-clean, then `retry_later` on timeout;
  `dt_remote_transform_ensure_fresh`), shared verbatim with
  `sample_region`. Note the angle probe works in isotropic pixels, not
  normalized space (Amendment 5).
- **GUI-edit-session guard** (hard-challenge H2, decision 8): before
  mutating or deleting any form, the engine checks whether that form (or
  its group) is the live edit target (`dev->form_visible` /
  `dev->form_gui`, which caches derived geometry of the form being
  dragged — `masks.c:1320`). If so, it first cancels the GUI edit
  session via the GUI's own escape path (`dt_masks_change_form_gui(NULL)`
  + redraw), then proceeds — matching the "remote drives the GUI"
  posture of the reveal behavior. Covered by a unit test on the guard
  path and a documented manual test; a use-after-free here is the
  predicted failure mode when a user meets the agent mid-drag.

### darktable-core additions

The core exports `dt_masks_group_create_for_module`, cycle-safe graph and
owner queries, and ownership-safe non-group full deletion. It also adds
explicit creation options and `dt_masks_gui_form_save_creation_ext`, plus
`dt_dev_add_new_masks_history_item` for forced-new masks snapshots.

### History, revision, undo

- Creation with attach commits **two** forced-new masks history items.
  The first contains the unattached form and unchanged module state; the
  second contains the membership and the `ENABLED|MASK` mode addition.
  Both preserve `module->enabled`. The revision tracker counts each
  synchronous delivery and the mutation result reports the final revision — the
  `create_module_instance` precedent (also 2 items) applies verbatim,
  including the force-bump fallback.
- **Undo granularity wart (accepted, documented):** remote `undo`
  reverts one history item; undoing a `create_mask_shape`+attach takes
  two undo calls (the GUI has the same layered behavior). The reference
  documents this per method ("this call produces N history items").
  Each forced-new item has its own no-coalesce undo boundary; batching
  the two items into one undo record is explicitly out of v1 scope.
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

Integration (using M-B's `show_mask` render as the primary observable —
direct assertions on the mask image; preview-diffs remain as
end-to-end confirmation):
- circle on exposure at a known position → mask render bright inside the
  circle, dark outside, and preview-diff confined to that region
  (inside/outside delta-*ratio* assertion — the spatial gate);
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

1. **Reuse `dt_masks_gui_form_save_creation_ext` headless** rather than
   re-deriving the sequence — explicit creation options provide distinct
   snapshots, preserve module enablement, and add `mask_mode` only with
   membership.
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
8. **Live GUI edit sessions are cancelled, not raced** (2026-07-19 scope
   reorg): remote form mutations first end any active edit of the target
   form via `dt_masks_change_form_gui(NULL)` — see Engine design.
   Challenge: a user mid-drag loses their in-progress gesture; the
   alternative (refusing with a retryable error) was rejected as worse
   for unattended agent operation but is cheap to revisit.

## Amendments (planning — 2026-07-20)

Recorded when the implementation plan
(`2026-07-19-darktable-mcp-drawn-masks-tier3.md`) was written and
source-verified. These are binding contracts; the plan header carries the
full normative text.

1. **Pipe-freshness contract (H3).** Any wire call that back-transforms
   coordinates between a mutation and pipe reprocess (and any such call on
   a freshly-loaded image) blocks until `dev->preview_pipe->status ==
   DT_DEV_PIXELPIPE_VALID`: if not already valid, enqueue one
   `dt_dev_process_preview` and poll every 5 ms up to
   `pixelpipe_synchronization_timeout` iterations (hard cap 2000 ≈ 10 s
   when the conf is non-positive), else fail `retry_later`
   (`DT_REMOTE_ERR_PIPE_NOT_READY`, retryable). Every mutation marks the
   preview pipe `DIRTY` synchronously before returning, so a
   mutate-then-transform in one client breath never reads stale-`VALID`
   geometry. Implemented once as `dt_remote_transform_ensure_fresh` in
   `remote_transform.c` and **shared verbatim with `sample_region`**.
   *Interactive cost (accepted):* the poll runs on the GTK main thread, so
   a co-located user's UI stalls for the wait window on the first
   coordinate call after a distortion-changing edit; the common
   already-`VALID` path returns immediately. Documented in the protocol
   reference.

2. **GUI-edit-session guard (H2 / decision 8).** Before mutating or
   deleting any form (or a group referencing it), the engine cancels a
   live GUI edit of that target via `dt_masks_change_form_gui(NULL)`,
   which clears `form_gui->points` and nulls `form_visible` before the
   free — so a user mid-drag on the deleted form has their gesture
   discarded cleanly (no use-after-free). Implemented as
   `dt_remote_masks_cancel_gui_edit_if_targeting`.

3. **H7 tool merge.** `attach_mask` + `detach_mask` merged into one
   `set_mask_attachment { op, instance, shape_id, attached, state?,
   inverted?, opacity?, expected_revision }` (18→17 tools);
   `create`/`update`/`delete`/`list` stay separate. Docstring token
   budgets enforced by a sidecar test.

4. **Omitted optional geometry members are rejected.** Create/update
   require **every** member of the type's geometry; a missing member is
   `invalid_value` / `missing_member`. No per-shape conf defaults. (Closes
   the omitted-member open question in favor of a deterministic wire.)

5. **Probe constants pinned + angle space corrected.** `size_mapping`
   spread threshold = **1 % relative** across the 4 probe-arm distances;
   angle-probe arm = **0.05 of the shorter image edge, applied as an
   isotropic pixel offset**. The angle handling is isotropic-pixel, **not
   normalized**, because darktable stores mask rotation as a geometric
   angle in square-pixel space (`ellipse.c:233-286` scales radii by
   `MIN(w,h)`); a normalized-space probe would skew rotations on
   non-square images (diverges only under *anisotropic* distortion —
   perspective/keystone; a plain rotation is conformal and round-trips
   either way).

6. **First-member combine-op rule.** The bottom-most (first) member of a
   module's group carries **no** combine op — nothing beneath it to
   combine with — mirroring `dt_masks_gui_form_save_creation`
   (`masks.c:401-406`) and the GUI. A `state` supplied on a
   `set_mask_attachment` that lands as the first member is accepted and
   **ignored**; `inverted` and `opacity` still apply to every member. This
   holds on re-attach too (the bottom member never gains an op).

7. **Numeric ranges pinned to GUI storage clamps.** Circle/ellipse
   `radius`/`border ∈ [0.0005, 1.0]`, ellipse `radius_limit = 1.0`
   (non-clone), gradient `compression ∈ (0, 1.0]`, `curvature ∈ [-2, 2]`
   (verified `circle.c`/`ellipse.c`/`gradient.c`). Wire-space center/anchor
   allowed in `[-0.5, 1.5]` (moderately off-canvas, mirroring the GUI).

8. **Legacy `dt_masks_form_remove` remains for detach and reset paths.**
   `(module, grp, form)` is membership-only detach for drawn shapes.
   `(NULL, NULL, form)` scans only immediate base-group members and does
   not retain unlinked objects, so remote full deletion instead uses
   `dt_masks_form_remove_shape_full`.

9. **JSON numeric coercion.** Wire scalars are read through a helper that
   accepts both `G_TYPE_DOUBLE` and `G_TYPE_INT64` (mirroring
   `remote_blend.c:760-761`); integer JSON (e.g. `"radius": 1`) is never
   misread as `0.0`. Regression-tested.

10. **Disabled-module reveal answered.** Mutations reveal the affected
    module iff **exactly one** module references the shape, **regardless of
    its enabled state**; `list_mask_shapes` reveals nothing.
    `duplicate_mask_shape` remains **deferred** (no workflow needs it yet).

## Explicitly unresolved (all closed — see Amendments above)

Every item below was resolved during planning; kept for traceability.

- ~~**The pipe-freshness contract**~~ → Amendment 1 (shared with
  `sample_region`).
- ~~Final numeric ranges per geometry member~~ → Amendment 7.
- ~~Probe algorithm details (arm length, 1 % spread placeholder)~~ →
  Amendment 5.
- ~~Reveal on a shape whose only user is a *disabled* module~~ →
  Amendment 10 (reveal iff exactly one referencing module, any enabled
  state).
- ~~`dt_masks_form_duplicate` / `duplicate_mask_shape`~~ → Amendment 10
  (deferred).
- ~~Honor conf defaults vs require every member~~ → Amendment 4 (require
  explicitly).
