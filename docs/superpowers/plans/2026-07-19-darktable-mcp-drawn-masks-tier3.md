# darktable MCP — Drawn Masks (Tier 3 / M-C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** List, create, edit, delete drawn mask shapes (circle, ellipse, gradient) and attach/detach them to module instances with combine state and per-member opacity, behind a new `mask_shapes` hello capability, using new wire methods that speak preview-normalized coordinates and back-transform to raw-normalized storage.

**Architecture:** A new shared pure helper `src/control/remote_transform.c` performs the preview↔raw point mapping (via `dt_dev_distort_transform/backtransform` on the live preview pipe) plus the scalar-size/angle probe algorithm, and owns the **pipe-freshness contract** (block-until-clean on `dev->preview_pipe->status`). A new engine file `src/control/remote_masks.c` holds per-type geometry (de)serializers, adversarial-input validation, the `used_by` walk, state-bit↔string mapping, membership upsert, the mask_mode bit management, and the **GUI-edit-session guard** (`dt_masks_change_form_gui(NULL)`). Shape creation uses `dt_masks_gui_form_save_creation_ext`; core exports provide cycle-safe ownership queries, ownership-aware full deletion, and group creation. `remote_edit.c` gains the ref-resolving/CAS/revision/reveal entry points; `remote_protocol.c` adds the capability, five handlers, and fixtures.

**Tech Stack:** C (GLib, json-glib, cmocka), Python (FastMCP sidecar, pytest). Builds on M-A (`blend_params`, `remote_blend.c`, the divergence manifest, `get_module_schema`'s `instance` arg) and M-B (`parametric_mask_params` + `mask_render`'s `show_mask` on `render_preview`) as if fully implemented.

**Spec:** `docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md`. The `mask_mode` transition appendix in `docs/superpowers/specs/2026-07-19-darktable-mcp-mask-support-design-candidates.md` is authoritative for mask_mode vocabulary and transitions. Hard challenges: `docs/superpowers/specs/2026-07-19-darktable-mcp-mask-support-hard-challenges.md` (H2, H3, H6, H7).

## Global Constraints

- Capability name: exactly `mask_shapes`, advertised in `hello`. All five drawn-mask methods require it; the sidecar refuses client-side per capability exactly as `curves`/`vectors`/`quantities` do.
- **Wire methods (five, post-H7 merge — see Design amendment 3):** `list_mask_shapes` (read-only), `create_mask_shape`, `update_mask_shape`, `delete_mask_shape`, `set_mask_attachment` (attach+detach merged). Method table flags mirror M-A: read-only methods `{requires_image TRUE, mutating FALSE, deferred FALSE}`; mutating methods `{TRUE, TRUE, FALSE}`.
- **Coordinate space:** the wire speaks **preview-normalized** `[0,1]²` over the full rendered image exactly as `render_preview`/`show_mask` frames it (the preview pipe's `processed_width`/`processed_height`). Every request and response geometry object carries `"space": "preview"`. Responses additionally carry `"raw_geometry"` (stored values verbatim) and, for sizes/angles, `"size_mapping": "exact" | "approximate"`.
- **v1 editable shape types (writable strings):** `"circle"`, `"ellipse"`, `"gradient"`. All other form types (`"path"`, `"brush"`, `"group"`, `"clone"`, AI object) are listed with `id`/`type`/`name`/`used_by`, no `geometry`, `"editable": false` — attachable but not creatable/editable.
- **Geometry members per type (wire names, exact):**
  - `circle`: `center [x,y]`, `radius`, `border`
  - `ellipse`: `center [x,y]`, `radius [a,b]`, `rotation` (degrees), `border`, `border_mode`: `"equidistant"` | `"proportional"`
  - `gradient`: `anchor [x,y]`, `rotation` (degrees), `compression`, `steepness`, `curvature`
- **Validation policy (finite first, then range).** All geometry floats must be finite. Wire-space pre-checks: `radius > 0`, ellipse `radius[a] > 0` and `radius[b] > 0`, `border ≥ 0`, `compression > 0`, `|curvature| ≤ 2`, `rotation` finite, `steepness` finite, `center`/`anchor` finite and within `[-0.5, 1.5]` (the "moderately off-canvas" allowance mirroring the GUI). After back-transform, the raw-normalized stored values must fall in the GUI storage clamps or the write is rejected `invalid_value`: circle/ellipse `radius ∈ [0.0005, 1.0]` (`MIN_CIRCLE_RADIUS`/`MIN_CIRCLE_BORDER` = `0.0005f`, `max_mask_size`/`max_mask_border` = `1.0f` for non-clone; `src/develop/masks/circle.c:29-30,120-121`; ellipse `radius_limit = 1.0f` for non-clone, `ellipse.c:433-434`), circle/ellipse `border ∈ [0.0005, 1.0]`, gradient `compression ∈ (0.0, 1.0]`, gradient `curvature ∈ [-2.0, 2.0]` (`gradient.c:166-183`).
- **Adversarial-input caps:** at most one shape per `create_mask_shape`; reject unknown geometry members; `name` length `< sizeof(form->name)` = 128; nothing invalid ever reaches `dev->forms`.
- **Group member states (storage `int state`, `masks.h:54-67`):** `USE`/`SHOW` always set; combine op is exactly one of `UNION`/`INTERSECTION`/`DIFFERENCE`/`EXCLUSION`; `INVERSE` toggled from `inverted`; `SUM` is brush-only (refused in v1). Wire `state` vocabulary: `"union" | "intersection" | "difference" | "exclusion"`. `opacity` clamped to `[0,1]`. **First-member rule:** the bottom-most (first) member of a group carries **no** combine op — there is nothing beneath it to combine with — mirroring `dt_masks_gui_form_save_creation` (`masks.c:401-406`) and the GUI, which hides the combine dropdown for it. A `state` supplied on an attach that lands as the first member is accepted and ignored (documented in the `set_mask_attachment` docstring, Task 9). `inverted` and `opacity` are honored on every member including the first.
- **mask_mode (authoritative appendix):** the `MASK` bit changes ONLY via `create_mask_shape`(+attach)/`set_mask_attachment`/`delete_mask_shape`. `drawn ↔ drawn+parametric` transitions are handled by M-B's blend surface, never here. `raster` is never writable. On attach (any non-raster row), OR `DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK` into `mask_mode` (preserving `CONDITIONAL`); on an R-bit row, fail `invalid_value`, `constraint: "raster_unsupported"`. On detach/delete emptying a module's group, clear `DEVELOP_MASK_MASK` from that module's `mask_mode`.
- **Error slugs (exact):** `not_found` (unknown shape id — NEW code, see Task 7), `unsupported_field` (unsupported/non-editable type, or attach to a module without blending), `invalid_value` (non-finite/out-of-policy geometry, `sum_is_brush_only`, `raster_unsupported`), `revision_conflict` (CAS), `retry_later` (pipe not fresh — NEW code, see Task 2/7). `details_json` = `{"parameter": "...", "constraint": "..."}`.
- All five methods run on the GTK main thread via the existing dispatch; all mutating ones take `expected_revision` CAS. Remediated remote create/update/full delete use forced-new `dt_dev_add_new_masks_history_item` snapshots (creation through the extended helper; full delete with module/global entries), while legacy and compatibility paths retain `dt_dev_add_masks_history_item`. Undo is covered by existing `DT_UNDO_DEVELOP` scope; per-method history-item counts are documented (Task 11).
- Every commit message ends with BOTH trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`
- Verification stack (mirrors M-A): `cmake --build build -j$(nproc)` → `ctest --test-dir build` → `cd tools/mcp && .venv/bin/pytest -q` → (integration tasks) `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`.

---

## Design amendments / contracts (binding; full normative text — applied to the design docs in Task 11)

These are contracts, not implementation details. Amendments 1 and 2 are the two PLANNING PREREQUISITES named in the design's status section.

### Amendment 1 — Pipe-freshness contract (H3), shared verbatim with `sample_region`

**Problem.** `dt_dev_distort_backtransform` walks `dev->preview_pipe`'s distortion pieces (`pipe->nodes`, populated by `dt_dev_pixelpipe_change` during reprocessing). Between a distortion-changing mutation and the pipe's reprocess completion — or on a freshly-loaded image — the pipe is unprocessed, mid-invalidation, or reflects *pre-mutation* geometry. The GUI self-corrects every redraw; a one-shot wire call would place a shape deterministically wrong with no redraw.

**Verified mechanism (source facts, all 2026-07-19).**
- Pipe state is `dev->preview_pipe->status`, a `dt_dev_pixelpipe_status_t` (`src/develop/pixelpipe_hb.h:95-101`): `DT_DEV_PIXELPIPE_DIRTY = 0` (history changed / image new), `DT_DEV_PIXELPIPE_RUNNING = 1`, `DT_DEV_PIXELPIPE_VALID = 2` (finished, valid result), `DT_DEV_PIXELPIPE_INVALID = 3`. **Clean = `VALID`.**
- **Every mutation marks the preview pipe DIRTY synchronously before returning.** `dt_dev_add_masks_history_item` calls `dt_dev_invalidate_all` (`develop.c:1536`); the module-history path calls it at `develop.c:1421-1424`; `dt_dev_invalidate_all` sets `preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY` (`develop.c:307-315`). Therefore a *mutate-distortion-then-back-transform in one client breath* sees `DIRTY` first, **never a stale `VALID`** — this closes H3's deterministic-staleness hole at the root.
- The preview pipe is reprocessed by a **background worker** (`dt_dev_process_preview` enqueues a `DT_CTL_WORKER_ZOOM_FILL` job; `develop.c:283-287`), independent of the GTK main thread. Enqueuing can be done directly (bypassing the redraw handler), so polling `status` from the blocked main thread does not deadlock the worker.

**Contract (block-until-clean, then `retry_later`).** Any wire call that back-transforms coordinates between a mutation and pipe reprocess (create/update, and `list_mask_shapes`' forward transform), and any such call on a freshly-loaded image, MUST ensure the preview pipe is clean *before* transforming:

1. If `dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID`, proceed immediately.
2. Otherwise enqueue one `dt_dev_process_preview(dev)` and poll `dev->preview_pipe->status` every `5000` µs (`dt_iop_nap(5000)`), for at most `dt_conf_get_int("pixelpipe_synchronization_timeout")` iterations (the same budget `_dev_wait_hash` uses; `develop.c:3790-3803`), capped at 2000 iterations (≈10 s) when the conf is non-positive.
3. On reaching `VALID`, proceed. On `INVALID` or timeout, fail with the new error code `DT_REMOTE_ERR_PIPE_NOT_READY` (wire slug `retry_later`, retryable), message `"preview pipe not ready; retry"`, no state changed.

**Interactive cost (accepted).** Because the poll loop runs on the GTK main thread, a co-located user's UI is unresponsive for the duration of the wait (worst case ≈ the `pixelpipe_synchronization_timeout` budget; the 2000-iteration ≈10 s hard cap applies only when that conf is non-positive/misconfigured). This is the deliberate trade for deterministic placement over a one-shot wire call — the alternative (returning `retry_later` immediately whenever the pipe is not already `VALID`) would make coordinate calls flaky right after any edit. The common case is fast: an unchanged, already-`VALID` pipe returns at step 1 with no wait. This freeze window MUST be documented in the protocol reference (Task 11, Step 1) so integrators expect a bounded stall on the first coordinate call after a distortion-changing edit.

This is implemented once as `dt_remote_transform_ensure_fresh(dev, error)` in `remote_transform.c` (Task 2) and called by every coordinate-touching engine path. **It is shared verbatim with `sample_region`** (`docs/superpowers/specs/2026-07-18-darktable-mcp-picker-and-sampling-candidates.md`); Task 11 records it there as "answered here". The exercising test is the "mutate-distortion-then-create-shape in one breath" integration gate (Task 10, gate 4) plus a unit test that a DIRTY-then-timeout path returns `retry_later` (Task 2).

### Amendment 2 — GUI-edit-session guard (H2)

**Problem.** `dev->form_visible` / `dev->form_gui` hold live editing state (`form_gui->points` caches derived geometry of the form being dragged; `masks.c:1320-1332`). A remote `update_mask_shape`/`delete_mask_shape`/detach on *that* form mutates or frees structures the GUI interaction code reads across event iterations — a use-after-free when a user meets the agent mid-drag.

**Contract (cancel, not race — design decision 8).** Before mutating or deleting any form (or a group referencing it), the engine checks whether that form is the live edit target and, if so, cancels the live GUI edit session via the GUI's own escape path, then proceeds:

- The target is "live" when `darktable.develop->form_visible != NULL` AND its `formid` equals the form's `formid` OR the form's group `formid` (a group edit targets members transitively) OR `darktable.develop->form_gui->formid == form->formid`.
- Cancel with `dt_masks_change_form_gui(NULL)` (`masks.c:1284-...`), which calls `dt_masks_clear_form_gui` (frees `form_gui->points` and resets caches) and sets `form_visible = NULL`, then fires `dt_dev_masks_selection_change`. After this returns there is no live edit target, so the subsequent mutation/free cannot be read by stale GUI state.
- **Mid-drag on the very form being deleted:** the guard runs on the GTK main thread, the same thread that would service the next motion event; because it fully clears `form_gui` and nulls `form_visible` *before* the form is freed, the next motion event sees `form_visible == NULL` and no-ops (the interaction handlers all early-return on a null form). The user's in-progress gesture is discarded (accepted cost per decision 8; the alternative retryable-error posture was rejected as worse for unattended operation).

Implemented as `dt_remote_masks_cancel_gui_edit_if_targeting(dev, form)` in `remote_masks.c` (Task 5), called at the top of every form mutation/removal path. Covered by a unit test that the guard cancels when `form_visible` matches and no-ops otherwise (Task 5), plus a documented manual test (Task 11): start a GUI drag of a circle, issue a remote `delete_mask_shape` on it, confirm no crash and the drag ends cleanly.

### Amendment 3 — H7 tool-merging decision: merge attach/detach, keep the rest

The design lists six methods; H7 asks to re-examine merging at planning. **Decision:**

- **Merge `attach_mask` + `detach_mask` into one `set_mask_attachment`.** Wire: `{ op, instance, shape_id, attached, state?, inverted?, opacity?, expected_revision }`. `attached: true` upserts membership (creates the module group + `mask_id` if missing, appends/updates the point-group member, ORs `ENABLED|MASK` into `mask_mode`); `attached: false` removes the membership (`dt_masks_form_remove(module, grp, form)`; shape survives if referenced elsewhere; clears `MASK` when the group empties). `state`/`inverted`/`opacity` are read only when `attached: true`. This is a natural on/off pair over the same `(op, instance, shape_id)` key and removes one tool (18→17). Response carries the shape's fresh `list_mask_shapes` entry + revision.
- **Keep `create_mask_shape`, `update_mask_shape`, `delete_mask_shape` separate; do NOT fold into a create-style dispatch.** Their required fields differ by mode (create needs `type`+geometry+optional `attach`; update needs `id`+geometry; delete needs only `id`); a single mode-switched tool trades marginal token savings for exactly the conditional-rule confusion H4 warns models handle poorly. Keep `list_mask_shapes` separate (read-only, no CAS).
- **Docstring token budget (H7):** the `create_mask_shape`/`update_mask_shape` pair (which carry the coordinate contract + `size_mapping` + shared-shape warning) ≤ 1500 tokens *combined* by cross-referencing rather than repeating the contract; `list_mask_shapes`, `delete_mask_shape`, `set_mask_attachment` ≤ 500 tokens each. Enforced by a sidecar test asserting each docstring's word count is under budget (Task 9).

**Rationale recorded for review to challenge:** merging attach/detach assumes agents reason in "is this shape on this module?" terms; if a future workflow needs asymmetric error handling per direction, splitting is cheap to revisit.

### Amendment 4 — Omitted optional geometry members are rejected (design open question)

Following the design's stated preference: `create_mask_shape`/`update_mask_shape` require **every** member of the type's geometry explicitly; a missing member is `invalid_value`, `constraint: "missing_member"`. No per-shape conf defaults are applied. (Challenge welcome; keeps the wire deterministic.)

### Amendment 5 — Probe algorithm constants pinned

`size_mapping` spread threshold: `1%` relative — with 4 probe-arm raw distances `d[0..3]`, `size_mapping = "approximate"` iff `(max(d) - min(d)) / mean(d) > 0.01`, else `"exact"`. Angle probe arm length (for `rotation` read/write where no radius exists, e.g. gradient): `0.05` of the **shorter image edge**, applied as an **isotropic pixel** offset (not a normalized offset), because darktable stores mask rotation as a geometric angle in square-pixel space (`ellipse.c:233-286` scales radii by `MIN(w,h)`). Measuring in normalized space would skew rotations on non-square images. Size probe arms: the requested preview-space `radius` (circle/ellipse `radius[0]`). Recorded here because the design flagged these as unresolved placeholders.

### Verified engine facts that shape the implementation (not contradictions, but load-bearing)

- Remote creation uses `dt_masks_gui_form_save_creation_ext` with explicit
  options. The first forced-new snapshot contains the unattached form and
  unchanged module state; the second contains membership and the
  `ENABLED|MASK` addition. Both preserve `module->enabled`.
- Legacy `dt_masks_form_remove` remains unchanged for detach/reset paths.
  Remote full deletion uses `dt_masks_form_remove_shape_full` so incoming
  references are removed transitively and retired objects remain owned.

---

## File structure

| File | Responsibility |
|---|---|
| `src/develop/masks/masks.c` / `masks.h` | export `dt_masks_group_create_for_module` (rename-and-export of `_group_create`, GUI call sites updated) (Task 1) |
| `src/control/remote_transform.h` / `.c` (NEW) | shared preview↔raw point mapping, probe size/angle algorithm, `dt_remote_transform_ensure_fresh` (freshness contract) (Task 2) |
| `src/control/remote_masks.h` / `.c` (NEW) | geometry (de)serializers, validation, `list`/`create`/`update`/`delete`/`attach` engine logic, `used_by` walk, state mapping, mask_mode management, GUI-edit guard (Tasks 3–6) |
| `src/control/remote_edit.h` / `.c` | new error codes; ref-resolving/CAS/revision/reveal entry points + result structs (Task 7) |
| `src/control/remote_protocol.h` / `.c` | `mask_shapes` capability, five handlers, calls-table members, fixtures (Task 8) |
| `src/CMakeLists.txt` | add `control/remote_transform.c`, `control/remote_masks.c` (Task 2, 3) |
| `src/tests/unittests/control/test_remote_transform.c` (NEW) + `CMakeLists.txt` | transform + freshness unit tests (Task 2) |
| `src/tests/unittests/control/test_remote_masks.c` (NEW) + `CMakeLists.txt` | geometry/validation/CRUD/attach/guard unit tests (Tasks 3–6) |
| `src/tests/unittests/control/test_remote_protocol.c` + `fixtures/*.json` | protocol tests + shared fixtures (Task 8) |
| `tools/mcp/src/darktable_mcp/server.py` + `tools/mcp/tests/test_tools.py` | five tools, capability gate, docstring budget (Task 9) |
| `tools/mcp/tests/integration/test_masks_tier3.py` (NEW) | live gates using `show_mask` (Task 10) |
| docs (protocol reference, supported-operations, drawn-masks design status, picker candidates open-questions, divergence manifest, README) | Task 11 |

---

### Task 1: Export `dt_masks_group_create_for_module` (core rename-and-export)

**Files:**
- Modify: `src/develop/masks.h` (declare in the masks API block near `dt_masks_gui_form_save_creation`, masks.h:627)
- Modify: `src/develop/masks/masks.c:304-314` (drop `static`, rename) and its **four** call sites: `masks.c:392,394` (pass `dev`) and `masks.c:1551,1587` (pass `darktable.develop`)
- Test: `src/tests/unittests/control/test_remote_masks.c` (created in Task 3 uses it; Task 1 adds a minimal linkage test here)

**Interfaces:**
- Produces: `dt_masks_form_t *dt_masks_group_create_for_module(dt_develop_t *dev, const dt_iop_module_t *module, const dt_masks_type_t type);` — creates a group form of `type` (`DT_MASKS_GROUP` or `DT_MASKS_GROUP | DT_MASKS_CLONE`), names it from the module, appends to `dev->forms`, sets `module->blend_params->mask_id = grp->formid`, returns the group. Behavior-identical to the former static `_group_create`; the `type` parameter is retained (not dropped) so the refactor is a pure rename-and-export, and so `set_mask_attachment` can pick the correct group type from the attached form.

- [ ] **Step 1: Write the failing linkage test**

Create `src/tests/unittests/control/test_remote_masks.c` (standard darktable GPL header, then):

```c
/*
 * cmocka unit tests for the Tier-3 drawn-mask surface (M-C).
 *  - Task 1: linkage of the exported dt_masks_group_create_for_module.
 *  - Tasks 3-6 append geometry/validation/CRUD/attach/guard sections.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "develop/masks.h"

#include <glib.h>

// Task 1: the symbol must be linkable (public, non-static). We only assert
// the declaration compiles and the pointer is non-NULL; behavioral tests
// that need a live dev/module arrive in Task 3 with the dt_init harness.
static void test_group_create_symbol_is_public(void **state)
{
  (void)state;
  void (*p)(void) = (void (*)(void))dt_masks_group_create_for_module;
  assert_non_null(p);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_group_create_symbol_is_public),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
```

Append to `src/tests/unittests/control/CMakeLists.txt` (after the `test_remote_blend` block, same shape):

```cmake
add_cmocka_test(test_remote_masks
                SOURCES test_remote_masks.c
                LINK_LIBRARIES lib_darktable cmocka)

target_compile_definitions(test_remote_masks PRIVATE
  DT_TEST_MODULEDIR="${CMAKE_BINARY_DIR}/lib/darktable")

if(WIN32)
    _copy_required_library(test_remote_masks lib_darktable)
endif(WIN32)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: link/compile FAILURE — `dt_masks_group_create_for_module` undeclared / undefined reference.

- [ ] **Step 3: Declare in `masks.h`**

Insert after the `dt_masks_gui_form_save_creation` declaration (masks.h:630):

```c
/** create a masks group form for `module` of `type`
 * (DT_MASKS_GROUP, optionally | DT_MASKS_CLONE), name it from the module,
 * append it to dev->forms, and set module->blend_params->mask_id to the new
 * group id. Formerly the static _group_create in masks.c; exported for the
 * remote-edit engine's set_mask_attachment upsert path (M-C). */
dt_masks_form_t *dt_masks_group_create_for_module(dt_develop_t *dev,
                                                  const struct dt_iop_module_t *module,
                                                  const dt_masks_type_t type);
```

- [ ] **Step 4: Rename-and-export in `masks.c`**

Replace `masks.c:304-306`:

```c
static dt_masks_form_t *_group_create(dt_develop_t *dev,
                                      const dt_iop_module_t *module,
                                      const dt_masks_type_t type)
```

with:

```c
dt_masks_form_t *dt_masks_group_create_for_module(dt_develop_t *dev,
                                                  const dt_iop_module_t *module,
                                                  const dt_masks_type_t type)
```

Update **all four** internal call sites. The two in `dt_masks_gui_form_save_creation` (`masks.c:392,394`) pass `dev`:

```c
        grp = dt_masks_group_create_for_module(dev, module, DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        grp = dt_masks_group_create_for_module(dev, module, DT_MASKS_GROUP);
```

The two remaining sites (`masks.c:1551` and `masks.c:1587`) pass `darktable.develop` — update both:

```c
    grp = dt_masks_group_create_for_module(darktable.develop, module, DT_MASKS_GROUP);
```

Then confirm nothing references the old name: `grep -n '_group_create' src/develop/masks/masks.c` must show only the (now-renamed) definition.

- [ ] **Step 5: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_masks --output-on-failure`
Expected: PASS (1 test). Full suite unaffected: `ctest --test-dir build`.

- [ ] **Step 6: Commit**

```bash
git add src/develop/masks.h src/develop/masks/masks.c \
        src/tests/unittests/control/test_remote_masks.c src/tests/unittests/control/CMakeLists.txt
git commit -m "refactor: export dt_masks_group_create_for_module for the remote-edit engine

Rename-and-export the static _group_create (behavior-identical): the M-C
set_mask_attachment upsert path needs a module group when none exists.
GUI call sites updated; a linkage test freezes the public symbol."
```
(with the two Global Constraints trailers appended)

---

### Task 2: `remote_transform.c` — point mapping, probe algorithm, freshness contract, new error code

**Files:**
- Create: `src/control/remote_transform.h`, `src/control/remote_transform.c`
- Modify: `src/CMakeLists.txt` (add `"control/remote_transform.c"` alphabetically before `"control/remote_vector.c"`)
- Modify: `src/control/remote_edit.h` (add `DT_REMOTE_ERR_PIPE_NOT_READY` and `DT_REMOTE_ERR_NOT_FOUND` to `dt_remote_error_code_t`)
- Create: `src/tests/unittests/control/test_remote_transform.c`
- Modify: `src/tests/unittests/control/CMakeLists.txt`

**Interfaces:**
- Consumes: `dt_dev_distort_transform`/`dt_dev_distort_backtransform` (`develop.h:565,570`), `dt_masks_get_image_size` (`masks.h:1169`), `dev->preview_pipe->status` (`pixelpipe_hb.h:192`), `dt_dev_process_preview` (`develop.h:382`), `dt_remote_error_t`/`dt_remote_error_new` (remote_edit.h).
- Produces (for Tasks 3–6):
  - `gboolean dt_remote_transform_ensure_fresh(dt_develop_t *dev, dt_remote_error_t **error);`
  - `void dt_remote_transform_preview_to_raw_point(dt_develop_t *dev, double px, double py, double *rx, double *ry);` (preview-normalized → raw-normalized center)
  - `void dt_remote_transform_raw_to_preview_point(dt_develop_t *dev, double rx, double ry, double *px, double *py);`
  - `gboolean dt_remote_transform_preview_to_raw_size(dt_develop_t *dev, double cx_prev, double cy_prev, double r_prev, double *r_raw_out, gboolean *exact_out);` (probe cross; `r_raw` normalized per storage convention = raw pixel distance / MIN(iwidth,iheight))
  - `gboolean dt_remote_transform_raw_to_preview_size(dt_develop_t *dev, double cx_raw, double cy_raw, double r_raw, double *r_prev_out, gboolean *exact_out);`
  - `gboolean dt_remote_transform_preview_to_raw_angle(dt_develop_t *dev, double cx_prev, double cy_prev, double deg_prev, double *deg_raw_out);`
  - `gboolean dt_remote_transform_raw_to_preview_angle(dt_develop_t *dev, double cx_raw, double cy_raw, double deg_raw, double *deg_prev_out);`
  - Constants: `#define DT_REMOTE_TRANSFORM_SPREAD 0.01` and `#define DT_REMOTE_TRANSFORM_ANGLE_ARM 0.05`.

- [ ] **Step 1: Add the two error codes**

In `src/control/remote_edit.h`, extend `dt_remote_error_code_t` (after `DT_REMOTE_ERR_SCOPE_FAILED`):

```c
  DT_REMOTE_ERR_NOT_FOUND,       // M-C: unknown mask shape id (wire "not_found")
  DT_REMOTE_ERR_PIPE_NOT_READY,  // M-C: preview pipe not clean (wire "retry_later", retryable)
```

- [ ] **Step 2: Write the failing tests**

Create `src/tests/unittests/control/test_remote_transform.c` (GPL header, then). Reuse the M-A `dt_init` harness idiom (scratch confdir, `--library :memory:`, `--moduledir DT_TEST_MODULEDIR`), a `dt_dev_init`'d dev with no distortion modules so the transform is identity:

```c
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <math.h>

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "control/remote_transform.h"
#include "control/remote_edit.h"

#include <glib.h>

static char *s_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_confdir = g_dir_make_tmp("test_remote_transform-XXXXXX", &gerror);
  if(!s_confdir) { g_error_free(gerror); return -1; }
  char *argv_override[] = {
    "test_remote_transform", "--configdir", s_confdir, "--library", ":memory:",
    "--moduledir", DT_TEST_MODULEDIR, "--conf", "write_sidecar_files=never", NULL };
  int argc_override = G_N_ELEMENTS(argv_override) - 1;
  return dt_init(argc_override, argv_override, FALSE, FALSE, NULL) ? -1 : 0;
}

static int harness_group_teardown(void **state)
{
  (void)state;
  dt_cleanup();
  if(s_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_confdir);
    if(system(cmd) != 0) fprintf(stderr, "could not remove %s\n", s_confdir);
    g_free(cmd);
    g_clear_pointer(&s_confdir, g_free);
  }
  return 0;
}

// With no distortion modules and an identity preview pipe of known size,
// preview<->raw point mapping is exact and size_mapping is "exact".
static void test_identity_point_roundtrip(void **state)
{
  (void)state;
  double rx = 0, ry = 0, px = 0, py = 0;
  dt_remote_transform_preview_to_raw_point(darktable.develop, 0.5, 0.5, &rx, &ry);
  dt_remote_transform_raw_to_preview_point(darktable.develop, rx, ry, &px, &py);
  assert_float_equal(px, 0.5, 1e-4);
  assert_float_equal(py, 0.5, 1e-4);
}

static void test_identity_size_is_exact(void **state)
{
  (void)state;
  double r_raw = 0; gboolean exact = FALSE;
  const gboolean ok = dt_remote_transform_preview_to_raw_size(
      darktable.develop, 0.5, 0.5, 0.1, &r_raw, &exact);
  assert_true(ok);
  assert_true(exact);
  assert_true(r_raw > 0.0);
}

// Fresh, valid pipe returns TRUE immediately; a DIRTY pipe with a
// non-positive sync timeout returns FALSE (retry_later) without hanging.
static void test_ensure_fresh_valid_passes(void **state)
{
  (void)state;
  darktable.develop->preview_pipe->status = DT_DEV_PIXELPIPE_VALID;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_transform_ensure_fresh(darktable.develop, &err));
  assert_null(err);
}

static void test_ensure_fresh_dirty_times_out_to_retry_later(void **state)
{
  (void)state;
  dt_conf_set_int("pixelpipe_synchronization_timeout", 0); // omit waiting
  darktable.develop->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  // gui_attached is FALSE in this fixture, so dt_dev_process_preview is a
  // no-op and the pipe never turns VALID -> deterministic timeout branch.
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_transform_ensure_fresh(darktable.develop, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_PIPE_NOT_READY);
  dt_remote_error_free(err);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_identity_point_roundtrip),
    cmocka_unit_test(test_identity_size_is_exact),
    cmocka_unit_test(test_ensure_fresh_valid_passes),
    cmocka_unit_test(test_ensure_fresh_dirty_times_out_to_retry_later),
  };
  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
```

Register in `CMakeLists.txt` (same block shape as Task 1's `test_remote_masks`, name `test_remote_transform`).

Note on the timeout test: `test_ensure_fresh_valid_passes` runs before the dirty test but both set `status` explicitly; when `pixelpipe_synchronization_timeout <= 0` the poll loop uses the 2000-iteration cap — to keep the test fast the implementation MUST treat a non-positive conf as "0 poll iterations, fail immediately if not already VALID" (see Step 4). Confirm this ordering when implementing.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `control/remote_transform.h` not found.

- [ ] **Step 4: Write `remote_transform.h` and `remote_transform.c`**

`remote_transform.h`:

```c
/* (darktable GPL header, Copyright (C) 2026) */

// Shared preview<->raw coordinate mapping + the pipe-freshness contract
// (drawn-masks design amendment 1, shared verbatim with sample_region).
// Points are exact (dt_dev_distort_[back]transform on the live preview
// pipe); scalar sizes/angles use the documented probe algorithm and are
// exact only under uniform distortion. Every function taking a dt_develop_t
// must run on the GTK main thread (same rule as the masks GUI handlers).

#pragma once

#include "control/remote_edit.h"

#include <glib.h>

G_BEGIN_DECLS

struct dt_develop_t;

#define DT_REMOTE_TRANSFORM_SPREAD 0.01     // size_mapping "approximate" threshold (amendment 5)
#define DT_REMOTE_TRANSFORM_ANGLE_ARM 0.05  // angle-probe arm as a fraction of MIN(w,h), isotropic px (amendment 5)

/** Block-until-clean freshness contract: TRUE when dev->preview_pipe->status
 * is DT_DEV_PIXELPIPE_VALID (waiting up to pixelpipe_synchronization_timeout
 * iterations of 5ms while enqueuing one preview reprocess); FALSE with
 * *error = DT_REMOTE_ERR_PIPE_NOT_READY (wire "retry_later") on INVALID or
 * timeout. A non-positive conf means "do not wait": pass iff already VALID. */
gboolean dt_remote_transform_ensure_fresh(struct dt_develop_t *dev, dt_remote_error_t **error);

void dt_remote_transform_preview_to_raw_point(struct dt_develop_t *dev,
                                              double px, double py, double *rx, double *ry);
void dt_remote_transform_raw_to_preview_point(struct dt_develop_t *dev,
                                              double rx, double ry, double *px, double *py);

/** preview-normalized radius at (cx_prev,cy_prev) -> raw-normalized length
 * (raw pixel distance / MIN(iwidth,iheight)). *exact_out is FALSE when the
 * four probe-arm raw distances spread by more than DT_REMOTE_TRANSFORM_SPREAD
 * (relative). Returns FALSE only if the underlying distort call fails. */
gboolean dt_remote_transform_preview_to_raw_size(struct dt_develop_t *dev,
                                                 double cx_prev, double cy_prev, double r_prev,
                                                 double *r_raw_out, gboolean *exact_out);
gboolean dt_remote_transform_raw_to_preview_size(struct dt_develop_t *dev,
                                                 double cx_raw, double cy_raw, double r_raw,
                                                 double *r_prev_out, gboolean *exact_out);

gboolean dt_remote_transform_preview_to_raw_angle(struct dt_develop_t *dev,
                                                  double cx_prev, double cy_prev, double deg_prev,
                                                  double *deg_raw_out);
gboolean dt_remote_transform_raw_to_preview_angle(struct dt_develop_t *dev,
                                                  double cx_raw, double cy_raw, double deg_raw,
                                                  double *deg_prev_out);

G_END_DECLS
```

`remote_transform.c`:

```c
/* (darktable GPL header) */

#include "control/remote_transform.h"

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"

#include <math.h>

// preview-normalized [0,1] -> preview pixel uses the processed preview size;
// raw-normalized uses iwidth/iheight (dt_masks_get_image_size, masks.h:1169).
static void _sizes(float *pw, float *ph, float *iw, float *ih)
{
  dt_masks_get_image_size(pw, ph, iw, ih);
}

gboolean dt_remote_transform_ensure_fresh(dt_develop_t *dev, dt_remote_error_t **error)
{
  if(!dev || !dev->preview_pipe)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL, "no preview pipe");
    return TRUE ? (void)0, FALSE : FALSE;
  }
  if(dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID) return TRUE;

  int nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
  if(nloop <= 0) nloop = 0;          // "do not wait" — pass only if already VALID
  else if(nloop > 2000) nloop = 2000;

  // enqueue one reprocess (no-op when !gui_attached); the worker turns the
  // pipe VALID independently of this main-thread poll loop.
  dt_dev_process_preview(dev);

  for(int n = 0; n < nloop; n++)
  {
    const dt_dev_pixelpipe_status_t st = dev->preview_pipe->status;
    if(st == DT_DEV_PIXELPIPE_VALID) return TRUE;
    if(st == DT_DEV_PIXELPIPE_INVALID) break;
    dt_iop_nap(5000);
  }

  if(error)
    *error = dt_remote_error_new(DT_REMOTE_ERR_PIPE_NOT_READY,
                                 "preview pipe not ready; retry");
  return FALSE;
}

void dt_remote_transform_preview_to_raw_point(dt_develop_t *dev,
                                              double px, double py, double *rx, double *ry)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  float pts[2] = { (float)(px * pw), (float)(py * ph) };
  dt_dev_distort_backtransform(dev, pts, 1);
  *rx = pts[0] / iw;
  *ry = pts[1] / ih;
}

void dt_remote_transform_raw_to_preview_point(dt_develop_t *dev,
                                              double rx, double ry, double *px, double *py)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  float pts[2] = { (float)(rx * iw), (float)(ry * ih) };
  dt_dev_distort_transform(dev, pts, 1);
  *px = pts[0] / pw;
  *py = pts[1] / ph;
}

// mean raw-space distance from a transformed center to 4 arm points, plus
// the exact/approximate verdict from the spread.
static gboolean _probe_raw_distance(dt_develop_t *dev,
                                    double cx_prev, double cy_prev, double r_prev,
                                    double *mean_out, gboolean *exact_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double denom = fmin(iw, ih);
  // center + up/down/left/right arms, in preview pixels
  float pts[10] = {
    (float)(cx_prev * pw),               (float)(cy_prev * ph),
    (float)(cx_prev * pw),               (float)((cy_prev - r_prev) * ph),
    (float)(cx_prev * pw),               (float)((cy_prev + r_prev) * ph),
    (float)((cx_prev - r_prev) * pw),    (float)(cy_prev * ph),
    (float)((cx_prev + r_prev) * pw),    (float)(cy_prev * ph),
  };
  if(!dt_dev_distort_backtransform(dev, pts, 5)) return FALSE;
  const double ccx = pts[0], ccy = pts[1];
  double d[4], sum = 0.0, dmin = INFINITY, dmax = 0.0;
  for(int k = 0; k < 4; k++)
  {
    const double dx = pts[2 + k * 2] - ccx, dy = pts[3 + k * 2] - ccy;
    d[k] = hypot(dx, dy);
    sum += d[k];
    if(d[k] < dmin) dmin = d[k];
    if(d[k] > dmax) dmax = d[k];
  }
  const double mean = sum / 4.0;
  *mean_out = (denom > 0.0) ? (mean / denom) : 0.0;
  if(exact_out)
    *exact_out = (mean > 0.0) ? ((dmax - dmin) / mean <= DT_REMOTE_TRANSFORM_SPREAD) : TRUE;
  return TRUE;
}

gboolean dt_remote_transform_preview_to_raw_size(dt_develop_t *dev,
                                                 double cx_prev, double cy_prev, double r_prev,
                                                 double *r_raw_out, gboolean *exact_out)
{
  return _probe_raw_distance(dev, cx_prev, cy_prev, r_prev, r_raw_out, exact_out);
}

gboolean dt_remote_transform_raw_to_preview_size(dt_develop_t *dev,
                                                 double cx_raw, double cy_raw, double r_raw,
                                                 double *r_prev_out, gboolean *exact_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double denom = fmin(iw, ih);
  const double r_raw_px = r_raw * denom;
  // 4 arms in raw pixels around the raw center, forward-transformed
  float pts[10] = {
    (float)(cx_raw * iw),                 (float)(cy_raw * ih),
    (float)(cx_raw * iw),                 (float)(cy_raw * ih - r_raw_px),
    (float)(cx_raw * iw),                 (float)(cy_raw * ih + r_raw_px),
    (float)(cx_raw * iw - r_raw_px),      (float)(cy_raw * ih),
    (float)(cx_raw * iw + r_raw_px),      (float)(cy_raw * ih),
  };
  if(!dt_dev_distort_transform(dev, pts, 5)) return FALSE;
  const double ccx = pts[0], ccy = pts[1];
  double sum = 0.0, dmin = INFINITY, dmax = 0.0;
  for(int k = 0; k < 4; k++)
  {
    const double dx = (pts[2 + k * 2] - ccx) / pw, dy = (pts[3 + k * 2] - ccy) / ph;
    const double dist = hypot(dx, dy);
    sum += dist;
    if(dist < dmin) dmin = dist;
    if(dist > dmax) dmax = dist;
  }
  const double mean = sum / 4.0;
  *r_prev_out = mean;
  if(exact_out) *exact_out = (mean > 0.0) ? ((dmax - dmin) / mean <= DT_REMOTE_TRANSFORM_SPREAD) : TRUE;
  return TRUE;
}

// angle probe: darktable stores mask rotation as a geometric angle in
// SQUARE-PIXEL image space — ellipse/gradient scale their radii by MIN(w,h)
// and apply the rotation matrix there (ellipse.c:233-286). The arm must
// therefore be built in isotropic pixels and the result angle measured
// directly in pixels; doing it in aspect-distorted normalized space would
// skew every rotation on a non-square image.
static gboolean _probe_angle(dt_develop_t *dev, gboolean forward,
                             double cx, double cy, double deg_in, double *deg_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double sx = forward ? iw : pw, sy = forward ? ih : ph;   // source pixel dims
  const double rad = deg_in * M_PI / 180.0;
  // arm length as a fraction of the shorter source edge, in pixels (isotropic)
  const double arm = DT_REMOTE_TRANSFORM_ANGLE_ARM * fmin(sx, sy);
  float pts[4] = {
    (float)(cx * sx),                     (float)(cy * sy),
    (float)(cx * sx + arm * cos(rad)),    (float)(cy * sy - arm * sin(rad)),
  };
  const gboolean ok = forward ? dt_dev_distort_transform(dev, pts, 2)
                              : dt_dev_distort_backtransform(dev, pts, 2);
  if(!ok) return FALSE;
  // measure the angle directly in destination pixels (isotropic, no aspect skew)
  const double dx = pts[2] - pts[0], dy = pts[3] - pts[1];
  *deg_out = atan2(-dy, dx) * 180.0 / M_PI;
  return TRUE;
}

gboolean dt_remote_transform_preview_to_raw_angle(dt_develop_t *dev,
                                                  double cx_prev, double cy_prev, double deg_prev,
                                                  double *deg_raw_out)
{
  return _probe_angle(dev, FALSE, cx_prev, cy_prev, deg_prev, deg_raw_out);
}

gboolean dt_remote_transform_raw_to_preview_angle(dt_develop_t *dev,
                                                  double cx_raw, double cy_raw, double deg_raw,
                                                  double *deg_prev_out)
{
  return _probe_angle(dev, TRUE, cx_raw, cy_raw, deg_raw, deg_prev_out);
}
```

Fix the deliberate typo in `ensure_fresh`'s early-return (it is written as a reminder that the null-pipe branch must `return FALSE`): the null branch body is simply:

```c
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL, "no preview pipe");
    return FALSE;
```

Register `"control/remote_transform.c"` in `src/CMakeLists.txt`.

- [ ] **Step 5: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_transform --output-on-failure`
Expected: PASS (4 tests).

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_transform.h src/control/remote_transform.c src/CMakeLists.txt \
        src/control/remote_edit.h \
        src/tests/unittests/control/test_remote_transform.c src/tests/unittests/control/CMakeLists.txt
git commit -m "feat: shared preview<->raw transform helper + pipe-freshness contract

remote_transform.c: exact point mapping via dt_dev_distort_[back]transform,
the probe-cross size/angle algorithm with size_mapping spread verdict, and
dt_remote_transform_ensure_fresh (block-until-clean on preview_pipe->status,
retry_later on timeout). Adds DT_REMOTE_ERR_NOT_FOUND / _PIPE_NOT_READY.
Shared verbatim with the future sample_region effort (H3)."
```
(plus trailers)

---

### Task 3: `remote_masks.c` core — geometry (de)serializers + validation

**Files:**
- Create: `src/control/remote_masks.h`, `src/control/remote_masks.c`
- Modify: `src/CMakeLists.txt` (add `"control/remote_masks.c"` alphabetically after `"control/remote_frame.c"`)
- Test: `src/tests/unittests/control/test_remote_masks.c` (append; add the `dt_init` harness like Task 2)

**Interfaces:**
- Consumes: `remote_transform.h`; masks structs (`dt_masks_point_circle_t` etc., `masks.h:146-203`); `dt_remote_error_t`.
- Produces (for Tasks 4–6):
  - `typedef enum { DT_REMOTE_SHAPE_CIRCLE, DT_REMOTE_SHAPE_ELLIPSE, DT_REMOTE_SHAPE_GRADIENT, DT_REMOTE_SHAPE_UNSUPPORTED } dt_remote_shape_kind_t;`
  - `dt_remote_shape_kind_t dt_remote_masks_kind_from_type(dt_masks_type_t t);`
  - `const char *dt_remote_masks_type_string(dt_masks_type_t t);` (`"circle"/"ellipse"/"gradient"/"path"/"brush"/"group"/"clone"/"object"`)
  - `gboolean dt_remote_masks_type_from_string(const char *s, dt_masks_type_t *out);` (only the three editable types)
  - `gboolean dt_remote_masks_geometry_validate(dt_masks_type_t type, JsonObject *geom, dt_remote_error_t **error);` (pure wire-space policy: finiteness, required members, ranges; no transform)
  - `gboolean dt_remote_masks_geometry_to_points(dt_develop_t *dev, dt_masks_type_t type, JsonObject *geom, void *point_out, dt_remote_error_t **error);` (validate → back-transform → fill the malloc'd point struct; also enforces raw storage clamps)
  - `JsonNode *dt_remote_masks_points_to_geometry(dt_develop_t *dev, dt_masks_form_t *form);` (raw points → preview geometry object with `space`/`size_mapping`; NULL for unsupported types)
  - `JsonNode *dt_remote_masks_points_to_raw_geometry(dt_masks_form_t *form);` (stored values verbatim)

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_masks.c` (add the `dt_init` group setup/teardown + `#include "control/remote_masks.h"`, `#include <json-glib/json-glib.h>`, `#include <math.h>`). Convert `main()` to use `harness_group_setup`/`harness_group_teardown` (copy from Task 2 verbatim, renamed `test_remote_masks-XXXXXX`). Tests:

```c
static JsonObject *_geom(const char *json)
{
  JsonParser *p = json_parser_new();
  json_parser_load_from_data(p, json, -1, NULL);
  JsonObject *o = json_object_ref(json_node_get_object(json_parser_get_root(p)));
  g_object_unref(p);
  return o;  // caller unrefs
}

static void test_type_string_mapping(void **state)
{
  (void)state;
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_CIRCLE), "circle");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_ELLIPSE), "ellipse");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_GRADIENT), "gradient");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_PATH), "path");
  assert_string_equal(dt_remote_masks_type_string(DT_MASKS_BRUSH), "brush");
  dt_masks_type_t t = 0;
  assert_true(dt_remote_masks_type_from_string("ellipse", &t));
  assert_int_equal(t, DT_MASKS_ELLIPSE);
  assert_false(dt_remote_masks_type_from_string("path", &t));   // not editable
  assert_false(dt_remote_masks_type_from_string("nonsense", &t));
}

static void test_validate_circle_ok_and_rejections(void **state)
{
  (void)state;
  dt_remote_error_t *err = NULL;

  JsonObject *ok = _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.03}");
  assert_true(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, ok, &err));
  assert_null(err);
  json_object_unref(ok);

  JsonObject *missing = _geom("{\"center\":[0.5,0.5],\"radius\":0.1}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, missing, &err));
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err); err = NULL;
  json_object_unref(missing);

  JsonObject *neg = _geom("{\"center\":[0.5,0.5],\"radius\":-0.1,\"border\":0.0}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, neg, &err));
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err); err = NULL;
  json_object_unref(neg);

  JsonObject *nan = _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.0,\"extra\":1}");
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, nan, &err)); // unknown member
  dt_remote_error_free(err); err = NULL;
  json_object_unref(nan);

  JsonObject *off = _geom("{\"center\":[2.0,0.5],\"radius\":0.1,\"border\":0.0}"); // > 1.5
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, off, &err));
  dt_remote_error_free(err); err = NULL;
  json_object_unref(off);
}

static void test_validate_gradient_curvature_bounds(void **state)
{
  (void)state;
  dt_remote_error_t *err = NULL;
  JsonObject *bad = _geom("{\"anchor\":[0.5,0.5],\"rotation\":10.0,\"compression\":0.5,"
                          "\"steepness\":0.0,\"curvature\":3.0}"); // |curvature| > 2
  assert_false(dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, bad, &err));
  dt_remote_error_free(err); err = NULL;
  json_object_unref(bad);
}

// integer JSON numbers (no decimal point, parsed as INT64) must coerce to
// double, not be misread as 0.0 — regression guard for json_node_get_double.
static void test_validate_accepts_integer_json(void **state)
{
  (void)state;
  dt_remote_error_t *err = NULL;
  // rotation/steepness/curvature arrive as JSON integers; center as int pair
  JsonObject *g = _geom("{\"anchor\":[0,1],\"rotation\":0,\"compression\":0.5,"
                        "\"steepness\":0,\"curvature\":0}");
  assert_true(dt_remote_masks_geometry_validate(DT_MASKS_GRADIENT, g, &err));
  assert_null(err);
  json_object_unref(g);

  // an integer radius must read as 1.0, not 0.0 — under the json_node_get_double
  // bug it would be misread as 0 and rejected by the radius>0 check.
  JsonObject *ci = _geom("{\"center\":[0.5,0.5],\"radius\":1,\"border\":0}"); // ints
  assert_true(dt_remote_masks_geometry_validate(DT_MASKS_CIRCLE, ci, &err));
  assert_null(err);
  json_object_unref(ci);
}

// Identity pipe: geometry -> points -> geometry round-trips within tolerance.
static void test_circle_points_roundtrip_identity(void **state)
{
  (void)state;
  dt_remote_error_t *err = NULL;
  JsonObject *g = _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.03}");
  dt_masks_point_circle_t pt = { 0 };
  assert_true(dt_remote_masks_geometry_to_points(darktable.develop, DT_MASKS_CIRCLE, g, &pt, &err));
  assert_null(err);
  assert_true(pt.radius > 0.0f && pt.radius <= 1.0f);
  json_object_unref(g);

  dt_masks_form_t form = { 0 };
  form.type = DT_MASKS_CIRCLE;
  form.points = g_list_append(NULL, &pt);
  JsonNode *back = dt_remote_masks_points_to_geometry(darktable.develop, &form);
  JsonObject *bo = json_node_get_object(back);
  JsonArray *c = json_object_get_array_member(bo, "center");
  assert_float_equal(json_array_get_double_element(c, 0), 0.5, 1e-3);
  assert_string_equal(json_object_get_string_member(bo, "size_mapping"), "exact");
  json_node_unref(back);
  g_list_free(form.points);
}
```

Register the new tests in `main()`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `control/remote_masks.h` not found.

- [ ] **Step 3: Write `remote_masks.h`**

```c
/* (darktable GPL header) */

// Tier-3 drawn-mask engine surface (mask_shapes capability). Geometry
// (de)serialization treats all wire input as adversarial (first remote
// surface feeding variable structures into pipeline-adjacent code): every
// value is finiteness- and range-checked before any dt_masks_create call,
// and never reaches dev->forms unvalidated. Coordinate mapping is delegated
// to remote_transform.h. Every function taking a dt_develop_t/dt_iop_module_t
// runs on the GTK main thread.
//
// CAVEAT: create and full delete use the dev-parameterized extended APIs.
// Detach retains legacy dt_masks_form_remove behavior, which is hardwired to
// darktable.develop; unit tests on a standalone fixture dev must repoint the
// global around that Task 6 path.

#pragma once

#include "control/remote_edit.h"
#include "control/remote_transform.h"
#include "develop/masks.h"

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

struct dt_develop_t;
struct dt_iop_module_t;

typedef enum dt_remote_shape_kind_t
{
  DT_REMOTE_SHAPE_CIRCLE,
  DT_REMOTE_SHAPE_ELLIPSE,
  DT_REMOTE_SHAPE_GRADIENT,
  DT_REMOTE_SHAPE_UNSUPPORTED,
} dt_remote_shape_kind_t;

dt_remote_shape_kind_t dt_remote_masks_kind_from_type(dt_masks_type_t t);
const char *dt_remote_masks_type_string(dt_masks_type_t t);
gboolean dt_remote_masks_type_from_string(const char *s, dt_masks_type_t *out);

/** pure wire-space policy validation (Global Constraints "Validation
 * policy"): required members present, finite, in wire range; unknown members
 * rejected. No transform. FALSE + *error (DT_REMOTE_ERR_INVALID_VALUE, or
 * DT_REMOTE_ERR_UNSUPPORTED_FIELD for a non-editable type) on failure. */
gboolean dt_remote_masks_geometry_validate(dt_masks_type_t type, JsonObject *geom,
                                           dt_remote_error_t **error);

/** validate, back-transform (preview->raw), and fill `point_out` (a caller
 * struct of the type's size). Enforces raw storage clamps after transform.
 * Requires a fresh pipe — caller must have run ensure_fresh first. */
gboolean dt_remote_masks_geometry_to_points(struct dt_develop_t *dev, dt_masks_type_t type,
                                            JsonObject *geom, void *point_out,
                                            dt_remote_error_t **error);

/** stored raw points -> preview-normalized geometry object (with "space",
 * "size_mapping"); NULL for unsupported/non-editable types. Caller owns. */
JsonNode *dt_remote_masks_points_to_geometry(struct dt_develop_t *dev, dt_masks_form_t *form);
/** stored raw points verbatim as a "raw_geometry" object; NULL for
 * unsupported types. Caller owns. */
JsonNode *dt_remote_masks_points_to_raw_geometry(dt_masks_form_t *form);

G_END_DECLS
```

- [ ] **Step 4: Write `remote_masks.c` (this task's half)**

Full implementation of the type mapping + validation + serializers. (Later tasks append the CRUD/attach/guard sections to the same file.)

```c
/* (darktable GPL header) */

#include "control/remote_masks.h"

#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <math.h>
#include <string.h>

// storage clamps (Global Constraints), verified from circle.c/ellipse.c/gradient.c
#define DTRM_SIZE_MIN 0.0005
#define DTRM_SIZE_MAX 1.0
#define DTRM_COMPRESSION_MIN 0.0   // exclusive; wire requires > 0
#define DTRM_COMPRESSION_MAX 1.0
#define DTRM_CURVATURE_ABS 2.0
#define DTRM_CENTER_LO (-0.5)
#define DTRM_CENTER_HI 1.5

dt_remote_shape_kind_t dt_remote_masks_kind_from_type(dt_masks_type_t t)
{
  if(t & DT_MASKS_CIRCLE) return DT_REMOTE_SHAPE_CIRCLE;
  if(t & DT_MASKS_ELLIPSE) return DT_REMOTE_SHAPE_ELLIPSE;
  if(t & DT_MASKS_GRADIENT) return DT_REMOTE_SHAPE_GRADIENT;
  return DT_REMOTE_SHAPE_UNSUPPORTED;
}

const char *dt_remote_masks_type_string(dt_masks_type_t t)
{
  if(t & DT_MASKS_CIRCLE) return "circle";
  if(t & DT_MASKS_ELLIPSE) return "ellipse";
  if(t & DT_MASKS_GRADIENT) return "gradient";
  if(t & DT_MASKS_PATH) return "path";
  if(t & DT_MASKS_BRUSH) return "brush";
  if(t & DT_MASKS_GROUP) return "group";
  if(t & DT_MASKS_CLONE) return "clone";
  return "unsupported";
}

gboolean dt_remote_masks_type_from_string(const char *s, dt_masks_type_t *out)
{
  if(!s) return FALSE;
  if(!strcmp(s, "circle")) { *out = DT_MASKS_CIRCLE; return TRUE; }
  if(!strcmp(s, "ellipse")) { *out = DT_MASKS_ELLIPSE; return TRUE; }
  if(!strcmp(s, "gradient")) { *out = DT_MASKS_GRADIENT; return TRUE; }
  return FALSE;
}

static gboolean _err_iv(dt_remote_error_t **e, const char *param, const char *constraint)
{
  if(e)
  {
    *e = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, "invalid geometry member '%s'", param);
    // details_json shape mirrors the M-A blend errors: {"parameter","constraint"}
    (*e)->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"constraint\":\"%s\"}",
                                         param, constraint);
  }
  return FALSE;
}

// coerce a JSON scalar node to double, accepting both real and integer JSON
// numbers (json_node_get_double is NOT safe on an INT64 node — mirror the M-A
// idiom at remote_blend.c:760-761). Returns FALSE if the node is not numeric.
static gboolean _node_num(JsonNode *n, double *v)
{
  if(!n || json_node_get_node_type(n) != JSON_NODE_VALUE) return FALSE;
  const GType t = json_node_get_value_type(n);
  if(t == G_TYPE_DOUBLE) { *v = json_node_get_double(n); return TRUE; }
  if(t == G_TYPE_INT64)  { *v = (double)json_node_get_int(n); return TRUE; }
  return FALSE;
}

// finite scalar member fetch
static gboolean _num(JsonObject *o, const char *k, double *v, dt_remote_error_t **e)
{
  if(!json_object_has_member(o, k)) return _err_iv(e, k, "missing_member");
  if(!_node_num(json_object_get_member(o, k), v)) return _err_iv(e, k, "not_a_number");
  if(!isfinite(*v)) return _err_iv(e, k, "not_finite");
  return TRUE;
}

static gboolean _point2(JsonObject *o, const char *k, double *x, double *y, dt_remote_error_t **e)
{
  if(!json_object_has_member(o, k)) return _err_iv(e, k, "missing_member");
  JsonNode *an = json_object_get_member(o, k);
  if(json_node_get_node_type(an) != JSON_NODE_ARRAY) return _err_iv(e, k, "expected_pair");
  JsonArray *a = json_node_get_array(an);
  if(!a || json_array_get_length(a) != 2) return _err_iv(e, k, "expected_pair");
  if(!_node_num(json_array_get_element(a, 0), x)
     || !_node_num(json_array_get_element(a, 1), y))
    return _err_iv(e, k, "not_a_number");
  if(!isfinite(*x) || !isfinite(*y)) return _err_iv(e, k, "not_finite");
  if(*x < DTRM_CENTER_LO || *x > DTRM_CENTER_HI || *y < DTRM_CENTER_LO || *y > DTRM_CENTER_HI)
    return _err_iv(e, k, "out_of_canvas");
  return TRUE;
}

// member accessors used by geometry_to_points — reuse the coercing fetch above
// so integer JSON (e.g. "rotation": 0) is never silently read as 0.0 garbage.
static double _memb(JsonObject *o, const char *k)   // caller guarantees validated+present
{
  double v = 0.0;
  _node_num(json_object_get_member(o, k), &v);
  return v;
}
static void _memb_pair(JsonObject *o, const char *k, double *a, double *b)
{
  JsonArray *arr = json_node_get_array(json_object_get_member(o, k));
  _node_num(json_array_get_element(arr, 0), a);
  _node_num(json_array_get_element(arr, 1), b);
}

// reject any member not in the allowed set for the type
static gboolean _reject_unknown(JsonObject *o, const char *const *allowed, dt_remote_error_t **e)
{
  GList *members = json_object_get_members(o);
  for(GList *m = members; m; m = m->next)
  {
    gboolean ok = FALSE;
    for(const char *const *a = allowed; *a; a++)
      if(!strcmp(m->data, *a)) { ok = TRUE; break; }
    if(!ok) { g_list_free(members); return _err_iv(e, m->data, "unknown_member"); }
  }
  g_list_free(members);
  return TRUE;
}

gboolean dt_remote_masks_geometry_validate(dt_masks_type_t type, JsonObject *geom,
                                           dt_remote_error_t **error)
{
  double x, y, v;
  switch(dt_remote_masks_kind_from_type(type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      static const char *const allow[] = { "center", "radius", "border", NULL };
      if(!_reject_unknown(geom, allow, error)) return FALSE;
      if(!_point2(geom, "center", &x, &y, error)) return FALSE;
      if(!_num(geom, "radius", &v, error)) return FALSE;
      if(v <= 0.0) return _err_iv(error, "radius", "must_be_positive");
      if(!_num(geom, "border", &v, error)) return FALSE;
      if(v < 0.0) return _err_iv(error, "border", "must_be_nonnegative");
      return TRUE;
    }
    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      static const char *const allow[] =
        { "center", "radius", "rotation", "border", "border_mode", NULL };
      if(!_reject_unknown(geom, allow, error)) return FALSE;
      if(!_point2(geom, "center", &x, &y, error)) return FALSE;
      if(!json_object_has_member(geom, "radius")) return _err_iv(error, "radius", "missing_member");
      JsonNode *rn = json_object_get_member(geom, "radius");
      if(json_node_get_node_type(rn) != JSON_NODE_ARRAY) return _err_iv(error, "radius", "expected_pair");
      JsonArray *r = json_node_get_array(rn);
      if(!r || json_array_get_length(r) != 2) return _err_iv(error, "radius", "expected_pair");
      double ra = 0, rb = 0;
      if(!_node_num(json_array_get_element(r, 0), &ra) || !_node_num(json_array_get_element(r, 1), &rb))
        return _err_iv(error, "radius", "not_a_number");
      if(!isfinite(ra) || !isfinite(rb)) return _err_iv(error, "radius", "not_finite");
      if(ra <= 0.0 || rb <= 0.0) return _err_iv(error, "radius", "must_be_positive");
      if(!_num(geom, "rotation", &v, error)) return FALSE;
      if(!_num(geom, "border", &v, error)) return FALSE;
      if(v < 0.0) return _err_iv(error, "border", "must_be_nonnegative");
      if(!json_object_has_member(geom, "border_mode"))
        return _err_iv(error, "border_mode", "missing_member");
      const char *bm = json_object_get_string_member(geom, "border_mode");
      if(g_strcmp0(bm, "equidistant") && g_strcmp0(bm, "proportional"))
        return _err_iv(error, "border_mode", "must_be_equidistant_or_proportional");
      return TRUE;
    }
    case DT_REMOTE_SHAPE_GRADIENT:
    {
      static const char *const allow[] =
        { "anchor", "rotation", "compression", "steepness", "curvature", NULL };
      if(!_reject_unknown(geom, allow, error)) return FALSE;
      if(!_point2(geom, "anchor", &x, &y, error)) return FALSE;
      if(!_num(geom, "rotation", &v, error)) return FALSE;
      if(!_num(geom, "compression", &v, error)) return FALSE;
      if(v <= 0.0) return _err_iv(error, "compression", "must_be_positive");
      if(!_num(geom, "steepness", &v, error)) return FALSE;
      if(!_num(geom, "curvature", &v, error)) return FALSE;
      if(fabs(v) > DTRM_CURVATURE_ABS) return _err_iv(error, "curvature", "abs_gt_2");
      return TRUE;
    }
    default:
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                     "shape type is not editable");
      return FALSE;
  }
}

static gboolean _clamp_ok(double v, double lo, double hi) { return v >= lo && v <= hi; }

gboolean dt_remote_masks_geometry_to_points(dt_develop_t *dev, dt_masks_type_t type,
                                            JsonObject *geom, void *point_out,
                                            dt_remote_error_t **error)
{
  if(!dt_remote_masks_geometry_validate(type, geom, error)) return FALSE;

  switch(dt_remote_masks_kind_from_type(type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      dt_masks_point_circle_t *c = point_out;
      double cx, cy, rx, ry, r_raw, b_raw; gboolean ex;
      _point2(geom, "center", &cx, &cy, error);
      dt_remote_transform_preview_to_raw_point(dev, cx, cy, &rx, &ry);
      c->center[0] = (float)rx; c->center[1] = (float)ry;
      dt_remote_transform_preview_to_raw_size(dev, cx, cy, _memb(geom, "radius"), &r_raw, &ex);
      dt_remote_transform_preview_to_raw_size(dev, cx, cy, _memb(geom, "border"), &b_raw, &ex);
      if(!_clamp_ok(r_raw, DTRM_SIZE_MIN, DTRM_SIZE_MAX)) return _err_iv(error, "radius", "out_of_range");
      if(!_clamp_ok(b_raw, 0.0, DTRM_SIZE_MAX)) return _err_iv(error, "border", "out_of_range");
      c->radius = (float)r_raw; c->border = (float)fmax(b_raw, DTRM_SIZE_MIN);
      return TRUE;
    }
    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      dt_masks_point_ellipse_t *e = point_out;
      double cx, cy, rx, ry, ra_raw, rb_raw, b_raw, rot_raw; gboolean ex;
      _point2(geom, "center", &cx, &cy, error);
      dt_remote_transform_preview_to_raw_point(dev, cx, cy, &rx, &ry);
      e->center[0] = (float)rx; e->center[1] = (float)ry;
      double ra_prev = 0, rb_prev = 0;
      _memb_pair(geom, "radius", &ra_prev, &rb_prev);
      dt_remote_transform_preview_to_raw_size(dev, cx, cy, ra_prev, &ra_raw, &ex);
      dt_remote_transform_preview_to_raw_size(dev, cx, cy, rb_prev, &rb_raw, &ex);
      if(!_clamp_ok(ra_raw, DTRM_SIZE_MIN, DTRM_SIZE_MAX)
         || !_clamp_ok(rb_raw, DTRM_SIZE_MIN, DTRM_SIZE_MAX))
        return _err_iv(error, "radius", "out_of_range");
      e->radius[0] = (float)ra_raw; e->radius[1] = (float)rb_raw;
      dt_remote_transform_preview_to_raw_angle(dev, cx, cy, _memb(geom, "rotation"), &rot_raw);
      e->rotation = (float)rot_raw;
      const gboolean prop =
        !g_strcmp0(json_object_get_string_member(geom, "border_mode"), "proportional");
      e->flags = prop ? DT_MASKS_ELLIPSE_PROPORTIONAL : DT_MASKS_ELLIPSE_EQUIDISTANT;
      // border is proportional (fraction) or a raw length depending on mode.
      const double bw = _memb(geom, "border");
      if(prop) { if(bw < 0.0) return _err_iv(error, "border", "out_of_range"); e->border = (float)bw; }
      else {
        dt_remote_transform_preview_to_raw_size(dev, cx, cy, bw, &b_raw, &ex);
        e->border = (float)fmax(b_raw, DTRM_SIZE_MIN);
      }
      return TRUE;
    }
    case DT_REMOTE_SHAPE_GRADIENT:
    {
      dt_masks_point_gradient_t *g = point_out;
      double ax, ay, rx, ry, rot_raw;
      _point2(geom, "anchor", &ax, &ay, error);
      dt_remote_transform_preview_to_raw_point(dev, ax, ay, &rx, &ry);
      g->anchor[0] = (float)rx; g->anchor[1] = (float)ry;
      dt_remote_transform_preview_to_raw_angle(dev, ax, ay, _memb(geom, "rotation"), &rot_raw);
      g->rotation = (float)rot_raw;
      const double comp = _memb(geom, "compression");
      if(!_clamp_ok(comp, DTRM_SIZE_MIN, DTRM_COMPRESSION_MAX)) return _err_iv(error, "compression", "out_of_range");
      g->compression = (float)comp;
      g->steepness = (float)_memb(geom, "steepness");
      g->curvature = (float)_memb(geom, "curvature");
      g->state = DT_MASKS_GRADIENT_STATE_SIGMOIDAL;  // GUI default for new gradients
      return TRUE;
    }
    default:
      if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "not editable");
      return FALSE;
  }
}

static void _add_point2(JsonObject *o, const char *k, double a, double b)
{
  JsonArray *arr = json_array_new();
  json_array_add_double_element(arr, a);
  json_array_add_double_element(arr, b);
  json_object_set_array_member(o, k, arr);
}

JsonNode *dt_remote_masks_points_to_geometry(dt_develop_t *dev, dt_masks_form_t *form)
{
  if(!form || !form->points) return NULL;
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "space", "preview");
  gboolean exact = TRUE, exq = TRUE;
  double px, py, rp;
  switch(dt_remote_masks_kind_from_type(form->type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    {
      const dt_masks_point_circle_t *c = form->points->data;
      dt_remote_transform_raw_to_preview_point(dev, c->center[0], c->center[1], &px, &py);
      _add_point2(o, "center", px, py);
      dt_remote_transform_raw_to_preview_size(dev, c->center[0], c->center[1], c->radius, &rp, &exact);
      json_object_set_double_member(o, "radius", rp);
      dt_remote_transform_raw_to_preview_size(dev, c->center[0], c->center[1], c->border, &rp, &exq);
      json_object_set_double_member(o, "border", rp);
      break;
    }
    case DT_REMOTE_SHAPE_ELLIPSE:
    {
      const dt_masks_point_ellipse_t *e = form->points->data;
      double dp;
      dt_remote_transform_raw_to_preview_point(dev, e->center[0], e->center[1], &px, &py);
      _add_point2(o, "center", px, py);
      JsonArray *r = json_array_new();
      dt_remote_transform_raw_to_preview_size(dev, e->center[0], e->center[1], e->radius[0], &rp, &exact);
      json_array_add_double_element(r, rp);
      dt_remote_transform_raw_to_preview_size(dev, e->center[0], e->center[1], e->radius[1], &rp, &exq);
      json_array_add_double_element(r, rp);
      json_object_set_array_member(o, "radius", r);
      exact = exact && exq;
      dt_remote_transform_raw_to_preview_angle(dev, e->center[0], e->center[1], e->rotation, &dp);
      json_object_set_double_member(o, "rotation", dp);
      json_object_set_string_member(o, "border_mode",
        e->flags == DT_MASKS_ELLIPSE_PROPORTIONAL ? "proportional" : "equidistant");
      if(e->flags == DT_MASKS_ELLIPSE_PROPORTIONAL) json_object_set_double_member(o, "border", e->border);
      else { dt_remote_transform_raw_to_preview_size(dev, e->center[0], e->center[1], e->border, &rp, &exq);
             json_object_set_double_member(o, "border", rp); }
      break;
    }
    case DT_REMOTE_SHAPE_GRADIENT:
    {
      const dt_masks_point_gradient_t *g = form->points->data;
      double dp;
      dt_remote_transform_raw_to_preview_point(dev, g->anchor[0], g->anchor[1], &px, &py);
      _add_point2(o, "anchor", px, py);
      dt_remote_transform_raw_to_preview_angle(dev, g->anchor[0], g->anchor[1], g->rotation, &dp);
      json_object_set_double_member(o, "rotation", dp);
      json_object_set_double_member(o, "compression", g->compression);
      json_object_set_double_member(o, "steepness", g->steepness);
      json_object_set_double_member(o, "curvature", g->curvature);
      break;
    }
    default:
      json_object_unref(o);
      return NULL;
  }
  json_object_set_string_member(o, "size_mapping", exact ? "exact" : "approximate");
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}

JsonNode *dt_remote_masks_points_to_raw_geometry(dt_masks_form_t *form)
{
  if(!form || !form->points) return NULL;
  JsonObject *o = json_object_new();
  switch(dt_remote_masks_kind_from_type(form->type))
  {
    case DT_REMOTE_SHAPE_CIRCLE:
    { const dt_masks_point_circle_t *c = form->points->data;
      _add_point2(o, "center", c->center[0], c->center[1]);
      json_object_set_double_member(o, "radius", c->radius);
      json_object_set_double_member(o, "border", c->border); break; }
    case DT_REMOTE_SHAPE_ELLIPSE:
    { const dt_masks_point_ellipse_t *e = form->points->data;
      _add_point2(o, "center", e->center[0], e->center[1]);
      JsonArray *r = json_array_new();
      json_array_add_double_element(r, e->radius[0]); json_array_add_double_element(r, e->radius[1]);
      json_object_set_array_member(o, "radius", r);
      json_object_set_double_member(o, "rotation", e->rotation);
      json_object_set_double_member(o, "border", e->border);
      json_object_set_string_member(o, "border_mode",
        e->flags == DT_MASKS_ELLIPSE_PROPORTIONAL ? "proportional" : "equidistant"); break; }
    case DT_REMOTE_SHAPE_GRADIENT:
    { const dt_masks_point_gradient_t *g = form->points->data;
      _add_point2(o, "anchor", g->anchor[0], g->anchor[1]);
      json_object_set_double_member(o, "rotation", g->rotation);
      json_object_set_double_member(o, "compression", g->compression);
      json_object_set_double_member(o, "steepness", g->steepness);
      json_object_set_double_member(o, "curvature", g->curvature); break; }
    default: json_object_unref(o); return NULL;
  }
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, o);
  return n;
}
```

**Note on `dt_remote_error_t::details_json`:** confirm the field name against `remote_edit.h` (M-A introduced `details_json` on the error struct for `{parameter, constraint}`). If M-A named it differently, use that name consistently across this plan.

Register `"control/remote_masks.c"` in `src/CMakeLists.txt`.

- [ ] **Step 4b: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_masks --output-on-failure`
Expected: PASS (all Task-1 + Task-3 tests).

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_masks.h src/control/remote_masks.c src/CMakeLists.txt \
        src/tests/unittests/control/test_remote_masks.c
git commit -m "feat: drawn-mask geometry (de)serializers + adversarial validation

remote_masks.c: type<->string mapping, pure wire-space policy validation
(finite, required members, ranges, unknown-member rejection), and
preview<->raw geometry conversion with raw storage-clamp enforcement and
size_mapping reporting. Nothing invalid reaches dev->forms."
```
(plus trailers)

---

### Task 4: `remote_masks.c` — `list_mask_shapes` (used_by walk + state mapping)

**Files:**
- Modify: `src/control/remote_masks.c` (append)
- Modify: `src/control/remote_masks.h` (append declarations)
- Test: `src/tests/unittests/control/test_remote_masks.c` (append)

**Interfaces:**
- Produces (for Tasks 7–8):
  - `const char *dt_remote_masks_state_op_string(int state);` (`"union"/"intersection"/"difference"/"exclusion"/"sum"`)
  - `gboolean dt_remote_masks_state_op_from_string(const char *s, int *op_bit_out);` (rejects `"sum"`)
  - `JsonNode *dt_remote_masks_list(struct dt_develop_t *dev);` (the `shapes` array; each entry = id/type/name/space/geometry?/raw_geometry?/editable/used_by; requires a fresh pipe — caller runs ensure_fresh)

- [ ] **Step 1: Write the failing tests**

Append to `test_remote_masks.c`. Build a fixture dev with an `exposure` module (blend fixture idiom, `blend_fixture_new`, copied from M-A `test_remote_blend.c`), create a circle attached to it via `dt_masks_gui_form_save_creation`, and assert the list output:

```c
static void test_state_op_mapping(void **state)
{
  (void)state;
  assert_string_equal(dt_remote_masks_state_op_string(DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION), "union");
  assert_string_equal(dt_remote_masks_state_op_string(DT_MASKS_STATE_INTERSECTION), "intersection");
  int op = 0;
  assert_true(dt_remote_masks_state_op_from_string("difference", &op));
  assert_int_equal(op, DT_MASKS_STATE_DIFFERENCE);
  assert_false(dt_remote_masks_state_op_from_string("sum", &op));   // brush-only
  assert_false(dt_remote_masks_state_op_from_string("bogus", &op));
}

static void test_list_reports_attached_circle(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  darktable.develop->preview_pipe->status = DT_DEV_PIXELPIPE_VALID;
  // NOTE: dt_masks_gui_form_save_creation uses darktable.develop; the blend
  // fixture installs fx->dev as a standalone dev. For this unit test we
  // build the form + membership directly on fx->dev (mirrors save_creation's
  // attached branch) so the list walk has data to report.
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_point_circle_t *c = malloc(sizeof(*c));
  *c = (dt_masks_point_circle_t){ .center = {0.5f, 0.5f}, .radius = 0.1f, .border = 0.03f };
  form->points = g_list_append(form->points, c);
  form->formid = 101;
  fx->dev.forms = g_list_append(fx->dev.forms, form);
  dt_masks_form_t *grp = dt_masks_group_create_for_module(&fx->dev, fx->module, DT_MASKS_GROUP);
  dt_masks_point_group_t *m = malloc(sizeof(*m));
  *m = (dt_masks_point_group_t){ .formid = 101, .parentid = grp->formid,
                                 .state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW | DT_MASKS_STATE_UNION,
                                 .opacity = 0.85f };
  grp->points = g_list_append(grp->points, m);

  JsonNode *node = dt_remote_masks_list(&fx->dev);
  JsonArray *shapes = json_object_get_array_member(json_node_get_object(node), "shapes");
  // the group form is NOT listed as a shape (decision 4)
  gboolean found = FALSE;
  for(guint i = 0; i < json_array_get_length(shapes); i++)
  {
    JsonObject *s = json_array_get_object_element(shapes, i);
    if(json_object_get_int_member(s, "id") == 101)
    {
      found = TRUE;
      assert_string_equal(json_object_get_string_member(s, "type"), "circle");
      assert_true(json_object_get_boolean_member(s, "editable"));
      JsonArray *ub = json_object_get_array_member(s, "used_by");
      assert_int_equal(json_array_get_length(ub), 1);
      JsonObject *u = json_array_get_object_element(ub, 0);
      assert_string_equal(json_object_get_string_member(u, "op"), "exposure");
      assert_float_equal(json_object_get_double_member(u, "opacity"), 0.85, 1e-4);
      JsonArray *st = json_object_get_array_member(u, "state");
      assert_string_equal(json_array_get_string_element(st, 0), "union");
    }
    assert_int_not_equal(json_object_get_int_member(s, "id"), grp->formid);
  }
  assert_true(found);
  json_node_unref(node);
  blend_fixture_free(fx);
}
```

Register the tests; copy the `blend_fixture_t`/`blend_fixture_new`/`blend_fixture_free` helpers from M-A's `test_remote_blend.c` into this file.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `dt_remote_masks_list` / `dt_remote_masks_state_op_string` undeclared.

- [ ] **Step 3: Implement (append to `remote_masks.c` + declare in `.h`)**

```c
const char *dt_remote_masks_state_op_string(int state)
{
  if(state & DT_MASKS_STATE_SUM) return "sum";
  if(state & DT_MASKS_STATE_INTERSECTION) return "intersection";
  if(state & DT_MASKS_STATE_DIFFERENCE) return "difference";
  if(state & DT_MASKS_STATE_EXCLUSION) return "exclusion";
  return "union";  // default; also when no op bit set
}

gboolean dt_remote_masks_state_op_from_string(const char *s, int *op_bit_out)
{
  if(!s) return FALSE;
  if(!strcmp(s, "union")) { *op_bit_out = DT_MASKS_STATE_UNION; return TRUE; }
  if(!strcmp(s, "intersection")) { *op_bit_out = DT_MASKS_STATE_INTERSECTION; return TRUE; }
  if(!strcmp(s, "difference")) { *op_bit_out = DT_MASKS_STATE_DIFFERENCE; return TRUE; }
  if(!strcmp(s, "exclusion")) { *op_bit_out = DT_MASKS_STATE_EXCLUSION; return TRUE; }
  return FALSE;  // "sum" (brush-only) and junk rejected
}

static const dt_masks_point_group_t *_find_membership(
  const dt_develop_t *dev,
  const dt_masks_form_t *group,
  const dt_mask_id_t id,
  GHashTable *visited)
{
  if(!dev || !group || !(group->type & DT_MASKS_GROUP)) return NULL;
  if(g_hash_table_contains(visited, group)) return NULL;
  g_hash_table_add(visited, (gpointer)group);

  for(GList *points = group->points;
      points;
      points = g_list_next(points))
  {
    const dt_masks_point_group_t *member = points->data;
    if(member && member->formid == id) return member;
  }
  for(GList *points = group->points;
      points;
      points = g_list_next(points))
  {
    const dt_masks_point_group_t *member = points->data;
    const dt_masks_form_t *child =
      member ? dt_masks_get_from_id(dev, member->formid) : NULL;
    const dt_masks_point_group_t *found =
      child ? _find_membership(dev, child, id, visited) : NULL;
    if(found) return found;
  }
  return NULL;
}

// Walk every blending module whose base group transitively contains id.
// _find_membership is cycle-safe and returns the target's nearest edge
// on the first depth-first path.
static void _append_used_by(dt_develop_t *dev, dt_mask_id_t id, JsonArray *out)
{
  for(GList *iops = dev->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *m = iops->data;
    if(!(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) continue;
    dt_masks_form_t *grp = dt_masks_get_from_id(dev, m->blend_params->mask_id);
    if(!grp || !dt_masks_group_contains_form(dev, grp, id)) continue;
    GHashTable *visited =
      g_hash_table_new(g_direct_hash, g_direct_equal);
    const dt_masks_point_group_t *gp =
      _find_membership(dev, grp, id, visited);
    g_hash_table_unref(visited);
    if(!gp) continue;
    JsonObject *u = json_object_new();
    json_object_set_string_member(u, "op", m->op);
    json_object_set_int_member(u, "instance", m->multi_priority);
    JsonArray *st = json_array_new();
    json_array_add_string_element(st, dt_remote_masks_state_op_string(gp->state));
    json_object_set_array_member(u, "state", st);
    json_object_set_boolean_member(u, "inverted", (gp->state & DT_MASKS_STATE_INVERSE) != 0);
    json_object_set_double_member(u, "opacity", gp->opacity);
    json_array_add_object_element(out, u);
  }
}

JsonNode *dt_remote_masks_list(dt_develop_t *dev)
{
  JsonObject *root = json_object_new();
  JsonArray *shapes = json_array_new();
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *form = l->data;
    if(form->type & DT_MASKS_GROUP) continue;  // groups are not wire objects (decision 4)
    JsonObject *s = json_object_new();
    json_object_set_int_member(s, "id", form->formid);
    json_object_set_string_member(s, "type", dt_remote_masks_type_string(form->type));
    json_object_set_string_member(s, "name", form->name);
    json_object_set_string_member(s, "space", "preview");
    const gboolean editable =
      dt_remote_masks_kind_from_type(form->type) != DT_REMOTE_SHAPE_UNSUPPORTED;
    json_object_set_boolean_member(s, "editable", editable);
    if(editable)
    {
      JsonNode *g = dt_remote_masks_points_to_geometry(dev, form);
      if(g) json_object_set_member(s, "geometry", g);
      JsonNode *rg = dt_remote_masks_points_to_raw_geometry(form);
      if(rg) json_object_set_member(s, "raw_geometry", rg);
    }
    JsonArray *ub = json_array_new();
    _append_used_by(dev, form->formid, ub);
    json_object_set_array_member(s, "used_by", ub);
    json_array_add_object_element(shapes, s);
  }
  json_object_set_array_member(root, "shapes", shapes);
  JsonNode *n = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(n, root);
  return n;
}
```

Add declarations to `remote_masks.h`.

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_masks --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_masks.c src/control/remote_masks.h src/tests/unittests/control/test_remote_masks.c
git commit -m "feat: list_mask_shapes engine — used_by walk + state-op mapping

Walks every blending module's group form to derive per-member combine
state/inverted/opacity; groups themselves are not listed (decision 4);
deferred types listed non-editable with no geometry."
```
(plus trailers)

---

### Task 5: `remote_masks.c` — create/update/delete shape + GUI-edit guard

> **Task 5 remediation (2026-07-24):** The original snippets below assumed
> flat module groups and set `mask_mode` before both creation snapshots.
> Those details are superseded by
> `2026-07-24-darktable-drawn-mask-task5-remediation.md`: use the
> cycle-safe core ownership queries, ownership-aware full deletion,
> transitive H2 guard, and extended creation helper. Binding Amendment 2
> remains transitive.

**Files:**
- Modify: `src/control/remote_masks.c` (append), `src/control/remote_masks.h`
- Test: `src/tests/unittests/control/test_remote_masks.c` (append)

**Interfaces:**
- Produces (for Task 7):
  - `void dt_remote_masks_cancel_gui_edit_if_targeting(struct dt_develop_t *dev, dt_masks_form_t *form);` (Amendment 2 guard)
  - `gboolean dt_remote_masks_create(struct dt_develop_t *dev, dt_masks_type_t type, JsonObject *geom, const char *name_or_null, struct dt_iop_module_t *attach_module_or_null, dt_mask_id_t *new_id_out, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_update(struct dt_develop_t *dev, dt_mask_id_t id, JsonObject *geom, const char *name_or_null, int *affects_out, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_delete(struct dt_develop_t *dev, dt_mask_id_t id, JsonArray *removed_from_out, dt_remote_error_t **error);`
  (all require a fresh pipe; caller runs `dt_remote_transform_ensure_fresh` first)

- [ ] **Step 1: Write the failing tests**

Append (using `blend_fixture_new` on `fx->dev`; note the guard tests
manipulate `darktable.develop->form_visible`). The H2 guard follows the
live GUI state on `darktable.develop`; in the unit harness, point that
global at the fixture dev for the duration. Concretely:

```c
static void test_guard_cancels_when_targeting(void **state)
{
  (void)state;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  form->formid = 202;
  // simulate a live GUI edit of this form
  darktable.develop->form_visible = form;
  darktable.develop->form_gui->formid = 202;
  dt_remote_masks_cancel_gui_edit_if_targeting(darktable.develop, form);
  assert_null(darktable.develop->form_visible);   // cancelled
  dt_masks_free_form(form);
}

static void test_guard_noop_when_not_targeting(void **state)
{
  (void)state;
  dt_masks_form_t *a = dt_masks_create(DT_MASKS_CIRCLE); a->formid = 1;
  dt_masks_form_t *b = dt_masks_create(DT_MASKS_CIRCLE); b->formid = 2;
  darktable.develop->form_visible = a;
  darktable.develop->form_gui->formid = 1;
  dt_remote_masks_cancel_gui_edit_if_targeting(darktable.develop, b);  // different form
  assert_ptr_equal(darktable.develop->form_visible, a);   // untouched
  darktable.develop->form_visible = NULL;
  dt_masks_free_form(a); dt_masks_free_form(b);
}

static void test_update_unknown_id_not_found(void **state)
{
  (void)state;
  darktable.develop->preview_pipe->status = DT_DEV_PIXELPIPE_VALID;
  dt_remote_error_t *err = NULL;
  JsonObject *g = _geom("{\"center\":[0.5,0.5],\"radius\":0.1,\"border\":0.0}");
  int affects = -1;
  assert_false(dt_remote_masks_update(darktable.develop, 99999, g, NULL, &affects, &err));
  assert_int_equal(err->code, DT_REMOTE_ERR_NOT_FOUND);
  dt_remote_error_free(err);
  json_object_unref(g);
}
```

(The end-to-end create→attach→mask_mode assertions live in Task 10's integration gates against a live darkroom, per the design's testing section; the unit layer covers the guard, not-found, and validation, which are the crash/correctness-critical paths.)

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — undeclared functions.

- [ ] **Step 3: Implement (append)**

```c
// Amendment 2 (H2): cancel a live GUI edit of `form` (or its group) before
// any mutation/free. Runs on the GTK main thread; after it returns there is
// no live edit target, so the subsequent free cannot be read by stale GUI
// state. dt_masks_change_form_gui uses darktable.develop internally.
void dt_remote_masks_cancel_gui_edit_if_targeting(dt_develop_t *dev, dt_masks_form_t *form)
{
  if(!dev || !form || dev != darktable.develop) return;
  const dt_masks_form_t *visible = dev->form_visible;
  if(!visible) return;
  const gboolean hit =
       visible->formid == form->formid
    || (dev->form_gui && dev->form_gui->formid == form->formid)
    || dt_masks_group_contains_form(dev, visible, form->formid);
  if(hit) dt_masks_change_form_gui(NULL);
}
```

```c
gboolean dt_remote_masks_create(dt_develop_t *dev, dt_masks_type_t type, JsonObject *geom,
                                const char *name_or_null, dt_iop_module_t *attach_module,
                                dt_mask_id_t *new_id_out, dt_remote_error_t **error)
{
  if(dt_remote_masks_kind_from_type(type) == DT_REMOTE_SHAPE_UNSUPPORTED)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "type is not creatable");
    return FALSE;
  }
  if(attach_module && (attach_module->blend_params->mask_mode & DEVELOP_MASK_RASTER))
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, "module has a raster mask");
      (*error)->details_json = g_strdup("{\"parameter\":\"op\",\"constraint\":\"raster_unsupported\"}");
    }
    return FALSE;
  }
  dt_masks_form_t *form = dt_masks_create(type);
  // one malloc'd point struct of the right size, filled from validated geometry
  size_t psize = (type & DT_MASKS_CIRCLE) ? sizeof(dt_masks_point_circle_t)
               : (type & DT_MASKS_ELLIPSE) ? sizeof(dt_masks_point_ellipse_t)
                                           : sizeof(dt_masks_point_gradient_t);
  void *pt = malloc(psize);
  if(!dt_remote_masks_geometry_to_points(dev, type, geom, pt, error))
  {
    free(pt);
    dt_masks_free_form(form);
    return FALSE;
  }
  form->points = g_list_append(form->points, pt);
  const dt_masks_form_creation_options_t options = {
    .requested_name = name_or_null,
    .preserve_module_enabled = TRUE,
    .mask_mode_to_add =
      attach_module ? DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK : 0,
  };
  dt_masks_gui_form_save_creation_ext(dev, attach_module, form, NULL,
                                      &options);
  if(new_id_out) *new_id_out = form->formid;
  return TRUE;
}

gboolean dt_remote_masks_update(dt_develop_t *dev, dt_mask_id_t id, JsonObject *geom,
                                const char *name_or_null, int *affects_out, dt_remote_error_t **error)
{
  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(!form)
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_NOT_FOUND, "no mask shape with id %d", id);
      (*error)->details_json = g_strdup("{\"parameter\":\"id\"}");
    }
    return FALSE;
  }
  if(dt_remote_masks_kind_from_type(form->type) == DT_REMOTE_SHAPE_UNSUPPORTED)
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "shape is not editable");
      (*error)->details_json = g_strdup("{\"parameter\":\"type\"}");
    }
    return FALSE;
  }
  // validate into a scratch struct FIRST (nothing invalid touches dev->forms)
  size_t psize = (form->type & DT_MASKS_CIRCLE) ? sizeof(dt_masks_point_circle_t)
               : (form->type & DT_MASKS_ELLIPSE) ? sizeof(dt_masks_point_ellipse_t)
                                                 : sizeof(dt_masks_point_gradient_t);
  void *scratch = malloc(psize);
  if(!dt_remote_masks_geometry_to_points(dev, form->type, geom, scratch, error))
  {
    free(scratch);
    return FALSE;
  }
  // H2 guard before mutating the live struct
  dt_remote_masks_cancel_gui_edit_if_targeting(dev, form);
  memcpy(form->points->data, scratch, psize);
  free(scratch);
  if(name_or_null && *name_or_null) g_strlcpy(form->name, name_or_null, sizeof(form->name));

  GPtrArray *owners =
    dt_masks_form_get_referencing_modules(dev, id);
  if(affects_out) *affects_out = owners->len;
  g_ptr_array_unref(owners);
  dt_dev_add_new_masks_history_item(dev, NULL, FALSE);
  return TRUE;
}

gboolean dt_remote_masks_delete(dt_develop_t *dev, dt_mask_id_t id,
                                JsonArray *removed_from_out, dt_remote_error_t **error)
{
  dt_masks_form_t *form = dt_masks_get_from_id(dev, id);
  if(!form)
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_NOT_FOUND, "no mask shape with id %d", id);
      (*error)->details_json = g_strdup("{\"parameter\":\"id\"}");
    }
    return FALSE;
  }
  dt_remote_masks_cancel_gui_edit_if_targeting(dev, form);

  GPtrArray *refs = NULL;
  if(!dt_masks_form_remove_shape_full(dev, form, &refs))
    return _operation_error(error, DT_REMOTE_ERR_INTERNAL,
                            "failed to delete mask shape", NULL, NULL);

  for(guint i = 0; i < refs->len; i++)
  {
    dt_iop_module_t *m = g_ptr_array_index(refs, i);
    if(removed_from_out)
    {
      JsonObject *r = json_object_new();
      json_object_set_string_member(r, "op", m->op);
      json_object_set_int_member(r, "instance", m->multi_priority);
      json_array_add_object_element(removed_from_out, r);
    }
  }
  g_ptr_array_unref(refs);
  return TRUE;
}
```

**Implementation note on `dt_masks_form_remove` args (verified `masks.c:1793-1892`):**
- **Detach (membership-only):** `dt_masks_form_remove(module, grp, form)` with `grp != NULL` and `form` lacking `CLONE|NON_CLONE` bits (all drawn shapes qualify) removes only the group membership; if the group then empties it recurses to remove the empty group. Used by `set_mask_attachment(attached:false)` (Task 6).
- **Full delete:** legacy `dt_masks_form_remove(NULL, NULL, form)` scans
  only immediate members of module base groups and does not retain
  unlinked objects. `delete_mask_shape` uses
  `dt_masks_form_remove_shape_full(dev, form, &references)` for
  transitive incoming-reference removal and ownership-safe retirement.

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_masks --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_masks.c src/control/remote_masks.h src/tests/unittests/control/test_remote_masks.c
git commit -m "feat: create/update/delete mask shape + GUI-edit-session guard (H2)

create uses dt_masks_gui_form_save_creation_ext for ordered snapshots
without enabling disabled modules; update validates into a scratch struct
and uses transitive ownership for affects_instances; delete uses
ownership-aware full removal. Every mutation first cancels a transitive
live GUI edit target via dt_masks_change_form_gui."
```
(plus trailers)

---

### Task 6: `remote_masks.c` — `set_mask_attachment` (upsert + detach + mask_mode)

**Files:**
- Modify: `src/control/remote_masks.c` (append), `src/control/remote_masks.h`
- Test: `src/tests/unittests/control/test_remote_masks.c` (append)

**Interfaces:**
- Produces (for Task 7):
  - `gboolean dt_remote_masks_set_attachment(struct dt_develop_t *dev, struct dt_iop_module_t *module, dt_mask_id_t shape_id, gboolean attached, const char *state_or_null, int inverted, /* -1 = leave */ const double *opacity_or_null, dt_remote_error_t **error);`

- [ ] **Step 1: Write the failing tests**

Append (fixture: `blend_fixture_new("exposure")`, an existing unattached circle in `fx->dev.forms`):

```c
static void test_attach_upsert_sets_mask_mode_and_member(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  darktable.develop->preview_pipe->status = DT_DEV_PIXELPIPE_VALID;

  // a first (bottom) shape — carries no combine op by the first-member rule
  dt_masks_form_t *base = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_point_circle_t *bc = malloc(sizeof(*bc));
  *bc = (dt_masks_point_circle_t){ .center = {0.4f,0.4f}, .radius = 0.1f, .border = 0.03f };
  base->points = g_list_append(base->points, bc);
  base->formid = 302;
  fx->dev.forms = g_list_append(fx->dev.forms, base);

  // a second shape whose combine op / inverted / opacity we assert
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_point_circle_t *c = malloc(sizeof(*c));
  *c = (dt_masks_point_circle_t){ .center = {0.5f,0.5f}, .radius = 0.1f, .border = 0.03f };
  form->points = g_list_append(form->points, c);
  form->formid = 303;
  fx->dev.forms = g_list_append(fx->dev.forms, form);

  dt_remote_error_t *err = NULL;
  // attach the base first so 303 lands as a non-first member
  assert_true(dt_remote_masks_set_attachment(&fx->dev, fx->module, 302, TRUE,
                                             "union", 0, NULL, &err));
  assert_null(err);
  const double op = 0.8;
  assert_true(dt_remote_masks_set_attachment(&fx->dev, fx->module, 303, TRUE,
                                             "intersection", 1, &op, &err));
  assert_null(err);
  // mask_mode gained ENABLED|MASK
  assert_true(fx->module->blend_params->mask_mode & DEVELOP_MASK_ENABLED);
  assert_true(fx->module->blend_params->mask_mode & DEVELOP_MASK_MASK);
  // group holds both members
  dt_masks_form_t *grp = dt_masks_get_from_id(&fx->dev, fx->module->blend_params->mask_id);
  assert_non_null(grp);
  // the bottom member (302) carries NO combine op (first-member rule)
  dt_masks_point_group_t *m302 = grp->points->data;
  assert_int_equal(m302->formid, 302);
  assert_int_equal(m302->state & DT_MASKS_STATE_OP, 0);
  // 303 carries the requested combine state / inverted / opacity
  dt_masks_point_group_t *m303 = NULL;
  for(GList *p = grp->points; p; p = g_list_next(p))
    if(((dt_masks_point_group_t *)p->data)->formid == 303) { m303 = p->data; break; }
  assert_non_null(m303);
  assert_true(m303->state & DT_MASKS_STATE_INTERSECTION);
  assert_true(m303->state & DT_MASKS_STATE_INVERSE);
  assert_float_equal(m303->opacity, 0.8, 1e-4);
  blend_fixture_free(fx);
}

static void test_attach_sum_refused(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE); form->formid = 304;
  dt_masks_point_circle_t *c = calloc(1, sizeof(*c)); c->radius = 0.1f;
  form->points = g_list_append(form->points, c);
  fx->dev.forms = g_list_append(fx->dev.forms, form);
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_masks_set_attachment(&fx->dev, fx->module, 304, TRUE, "sum", 0, NULL, &err));
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);
  blend_fixture_free(fx);
}

static void test_detach_empties_group_clears_mask_bit(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // dt_masks_form_remove (reached by the detach path) is hardwired to
  // darktable.develop; point it at the fixture dev so the empty-group cleanup
  // and mask_id clear land on fx->dev, then restore before freeing.
  dt_develop_t *saved_dev = darktable.develop;
  darktable.develop = &fx->dev;
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_CIRCLE); form->formid = 305;
  dt_masks_point_circle_t *c = calloc(1, sizeof(*c)); c->radius = 0.1f;
  form->points = g_list_append(form->points, c);
  fx->dev.forms = g_list_append(fx->dev.forms, form);
  const double op = 0.5;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_masks_set_attachment(&fx->dev, fx->module, 305, TRUE, "union", 0, &op, &err));
  assert_true(fx->module->blend_params->mask_mode & DEVELOP_MASK_MASK);
  assert_true(dt_remote_masks_set_attachment(&fx->dev, fx->module, 305, FALSE, NULL, -1, NULL, &err));
  assert_false(fx->module->blend_params->mask_mode & DEVELOP_MASK_MASK);  // cleared
  darktable.develop = saved_dev;
  blend_fixture_free(fx);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -5`
Expected: FAIL — `dt_remote_masks_set_attachment` undeclared.

- [ ] **Step 3: Implement (append)**

```c
gboolean dt_remote_masks_set_attachment(dt_develop_t *dev, dt_iop_module_t *module,
                                        dt_mask_id_t shape_id, gboolean attached,
                                        const char *state_or_null, int inverted,
                                        const double *opacity_or_null, dt_remote_error_t **error)
{
  if(!(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING))
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "module does not support masks");
      (*error)->details_json = g_strdup("{\"parameter\":\"op\"}");
    }
    return FALSE;
  }
  dt_masks_form_t *form = dt_masks_get_from_id(dev, shape_id);
  if(!form)
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_NOT_FOUND, "no mask shape with id %d", shape_id);
      (*error)->details_json = g_strdup("{\"parameter\":\"shape_id\"}");
    }
    return FALSE;
  }

  if(!attached)
  {
    dt_remote_masks_cancel_gui_edit_if_targeting(dev, form);
    dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
    dt_masks_form_remove(module, grp, form);  // membership-only removal branch
    if(dt_masks_get_from_id(dev, module->blend_params->mask_id) == NULL)
      module->blend_params->mask_mode &= ~DEVELOP_MASK_MASK;
    return TRUE;
  }

  // attached == TRUE: raster guard
  if(module->blend_params->mask_mode & DEVELOP_MASK_RASTER)
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, "module has a raster mask");
      (*error)->details_json = g_strdup("{\"parameter\":\"op\",\"constraint\":\"raster_unsupported\"}");
    }
    return FALSE;
  }
  int op_bit = DT_MASKS_STATE_UNION;
  if(state_or_null && !dt_remote_masks_state_op_from_string(state_or_null, &op_bit))
  {
    if(error)
    {
      *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, "bad combine state");
      (*error)->details_json =
        g_strcmp0(state_or_null, "sum") == 0
          ? g_strdup("{\"parameter\":\"state\",\"constraint\":\"sum_is_brush_only\"}")
          : g_strdup("{\"parameter\":\"state\",\"constraint\":\"unknown_state\"}");
    }
    return FALSE;
  }

  dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
  if(!grp)
    grp = dt_masks_group_create_for_module(dev, module,
            (form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) ? DT_MASKS_GROUP | DT_MASKS_CLONE
                                                                 : DT_MASKS_GROUP);

  // upsert the membership
  dt_masks_point_group_t *member = NULL;
  for(GList *p = grp->points; p; p = g_list_next(p))
    if(((dt_masks_point_group_t *)p->data)->formid == shape_id) { member = p->data; break; }
  const gboolean is_new = (member == NULL);
  // The bottom-most (first) member of a group carries NO combine op — there is
  // nothing beneath it to combine with. Mirrors save_creation (masks.c:401-406)
  // and the GUI (which hides the combine dropdown for it). A `state` that lands
  // on the first member is ignored (First-member rule, Global Constraints).
  if(is_new)
  {
    member = malloc(sizeof(dt_masks_point_group_t));
    member->formid = shape_id;
    member->parentid = grp->formid;
    member->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    member->state = DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW;
    if(grp->points) member->state |= op_bit;   // has a member beneath -> not first
    grp->points = g_list_append(grp->points, member);
  }
  else
  {
    // re-attach of an existing member: keep the first member op-less too
    const gboolean is_first = (grp->points && grp->points->data == member);
    member->state = (member->state & ~DT_MASKS_STATE_OP)
                    | DT_MASKS_STATE_USE | DT_MASKS_STATE_SHOW;
    if(!is_first) member->state |= op_bit;
  }
  if(inverted == 1) member->state |= DT_MASKS_STATE_INVERSE;
  else if(inverted == 0) member->state &= ~DT_MASKS_STATE_INVERSE;
  if(opacity_or_null) member->opacity = (float)CLAMP(*opacity_or_null, 0.0, 1.0);

  module->blend_params->mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
  dt_dev_add_masks_history_item(dev, module, TRUE);
  return TRUE;
}
```

**Note:** `DT_MASKS_STATE_OP` is the union of the five op bits (`masks.h:63-67`); masking it off before re-applying keeps the state clean on re-attach. `op_bit` is applied only to non-first members, matching darktable's rule that the bottom member of a group has no combine op (see First-member rule in Global Constraints).

- [ ] **Step 4: Build and run**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_masks --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_masks.c src/control/remote_masks.h src/tests/unittests/control/test_remote_masks.c
git commit -m "feat: set_mask_attachment — upsert/detach membership + mask_mode bit

attached=true creates the module group if needed, upserts the point-group
member (combine op / inverted / opacity), ORs ENABLED|MASK; attached=false
removes the membership and clears MASK when the group empties. sum refused."
```
(plus trailers)

---

### Task 7: Engine wiring in `remote_edit.c` — entry points, CAS, revision, reveal, result structs

**Files:**
- Modify: `src/control/remote_edit.h`, `src/control/remote_edit.c`
- Modify: `src/control/remote_protocol.c` (error slugs — see Step 5)
- Test: covered by Task 6's unit tests plus Task 8's protocol tests and Task 10's integration gates (the live entry path needs a darkroom).

**Interfaces:**
- Consumes: `dt_remote_masks_*` (Tasks 3–6), `dt_remote_transform_ensure_fresh` (Task 2), the existing static `dt_remote_require_darkroom_image`, `dt_remote_find_module`, `dt_remote_revision_*`, `_remote_reveal_module`.
- Produces (for Task 8):
  - `gboolean dt_remote_masks_list_json(JsonNode **out, uint64_t *revision, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_create_call(dt_masks_type_t type, JsonObject *geom, const char *name, const dt_remote_module_ref_t *attach_ref_or_null, const uint64_t *expected_revision, JsonNode **entry_out, uint64_t *revision, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_update_call(dt_mask_id_t id, JsonObject *geom, const char *name, const uint64_t *expected_revision, JsonNode **entry_out, int *affects_out, uint64_t *revision, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_delete_call(dt_mask_id_t id, const uint64_t *expected_revision, JsonArray **removed_from_out, uint64_t *revision, dt_remote_error_t **error);`
  - `gboolean dt_remote_masks_attachment_call(const dt_remote_module_ref_t *ref, dt_mask_id_t shape_id, gboolean attached, const char *state, int inverted, const double *opacity, const uint64_t *expected_revision, JsonNode **entry_out, uint64_t *revision, dt_remote_error_t **error);`
  (`entry_out` = the shape's fresh `list_mask_shapes` entry, built via a small helper that re-runs the list walk for one id)

- [ ] **Step 1: Declare the entry points in `remote_edit.h`**

Add after the blend `_for_ref` wrappers, with a header comment noting they run on the GTK main thread, do CAS/revision, and reveal the affected module when exactly one is affected. Include `#include "develop/masks.h"` for `dt_masks_type_t` / `dt_mask_id_t` and `#include <json-glib/json-glib.h>` (already added by M-A).

- [ ] **Step 2: Implement in `remote_edit.c`**

Add `#include "control/remote_masks.h"` and `#include "control/remote_transform.h"`. Each mutating entry point follows the established mutation shape (mirror `dt_remote_reset_module`, remote_edit.c:1659):

```c
// shared helper: the single-shape entry for a fresh list walk
static JsonNode *_masks_entry_for_id(dt_develop_t *dev, dt_mask_id_t id)
{
  JsonNode *list = dt_remote_masks_list(dev);
  JsonArray *shapes = json_object_get_array_member(json_node_get_object(list), "shapes");
  JsonNode *out = NULL;
  for(guint i = 0; i < json_array_get_length(shapes); i++)
  {
    JsonObject *s = json_array_get_object_element(shapes, i);
    if(json_object_get_int_member(s, "id") == id)
    {
      out = json_node_copy(json_array_get_element(shapes, i));
      break;
    }
  }
  json_node_unref(list);
  return out;
}

gboolean dt_remote_masks_list_json(JsonNode **out, uint64_t *revision, dt_remote_error_t **error)
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));
  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;
  dt_remote_revision_observe_image(dev->image_storage.id);
  if(!dt_remote_transform_ensure_fresh(dev, error)) return FALSE;   // freshness contract
  *out = dt_remote_masks_list(dev);
  if(revision) *revision = dt_remote_revision_get(dt_remote_revision_current());
  return TRUE;
}

gboolean dt_remote_masks_create_call(dt_masks_type_t type, JsonObject *geom, const char *name,
                                     const dt_remote_module_ref_t *attach_ref,
                                     const uint64_t *expected_revision, JsonNode **entry_out,
                                     uint64_t *revision, dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_mutation_precheck(expected_revision, &dev, error)) return FALSE;
  if(!dt_remote_transform_ensure_fresh(dev, error)) return FALSE;
  dt_iop_module_t *module = NULL;
  if(attach_ref)
  {
    module = dt_remote_find_module(dev, attach_ref, error);
    if(!module) return FALSE;
  }
  const uint64_t pre = dt_remote_revision_get(dt_remote_revision_current());
  dt_mask_id_t new_id = 0;
  if(!dt_remote_masks_create(dev, type, geom, name, module, &new_id, error)) return FALSE;
  if(module) _remote_reveal_module(module);
  if(entry_out) *entry_out = _masks_entry_for_id(dev, new_id);
  if(revision) *revision = dt_remote_read_new_revision(pre);
  return TRUE;
}
```

Implement `_update_call`, `_delete_call`, `_attachment_call` in the same shape: precheck (CAS) → `ensure_fresh` → resolve module (attachment) → call the `remote_masks` function → reveal when exactly one module affected (update: reveal iff `affects == 1`; attachment: always reveal `module`; delete: reveal none — the shape is gone) → build `entry_out` (delete has no entry; returns `removed_from`) → `dt_remote_read_new_revision(pre)`.

- [ ] **Step 3: Wire error slugs in `remote_protocol.c`**

Extend `_error_code_to_wire` (remote_protocol.c:2465):

```c
    case DT_REMOTE_ERR_NOT_FOUND: return "not_found";
    case DT_REMOTE_ERR_PIPE_NOT_READY: return "retry_later";
```

And mark `retry_later` retryable in the retryable-classification block near remote_protocol.c:2486 (alongside `revision_conflict`).

- [ ] **Step 4: Build and run all unit suites**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_edit.h src/control/remote_edit.c src/control/remote_protocol.c
git commit -m "feat: wire drawn-mask entry points — CAS, freshness, reveal, revision

remote_edit gains list/create/update/delete/attachment entry points that
run the freshness contract before any transform, do expected_revision CAS,
reveal the affected module when exactly one is touched, and return the
final revision. not_found/retry_later error slugs mapped."
```
(plus trailers)

---

### Task 8: Protocol layer — capability, five handlers, calls table, fixtures

**Files:**
- Modify: `src/control/remote_protocol.h` (calls table), `src/control/remote_protocol.c` (hello, handlers, method table)
- Create: fixtures under `src/tests/unittests/control/fixtures/`: `list_mask_shapes_request/response.json`, `create_mask_shape_request/response.json`, `update_mask_shape_request/response.json`, `delete_mask_shape_request/response.json`, `set_mask_attachment_request/response.json`, `create_mask_shape_error_not_found_request/response.json` (use an update error), and `set_mask_attachment_error_sum_request/response.json`
- Test: `src/tests/unittests/control/test_remote_protocol.c` (append), `tools/mcp/tests/test_protocol.py` (extend parametrization)

**Interfaces:**
- Consumes: Task 7's entry points.
- Produces: five wire methods gated on `mask_shapes`, calls-table members for stubbing.

- [ ] **Step 1: Write the failing C protocol tests**

Append to `test_remote_protocol.c`, following the file's stub-calls + `_assert_dispatch_matches()` idiom (mirror the neighbouring curve/quantity sections; calls-table type/installer are `dt_remote_protocol_calls_t` / `dt_remote_protocol_set_calls()`):

1. `test_hello_advertises_mask_shapes` — extend the hello test's expected-capabilities list with `"mask_shapes"` and update `fixtures/hello_response.json`.
2. `test_list_mask_shapes_round_trip` — stub `masks_list` returning a parsed 1-shape object; `_assert_dispatch_matches("list_mask_shapes_request.json", "list_mask_shapes_response.json")`.
3. `test_create_mask_shape_round_trip` — stub `masks_create` asserting `type == "circle"`, geometry present, attach ref `{op:"exposure", instance:0}`; returns a canned entry + revision; `_assert_dispatch_matches(create pair)`.
4. `test_update_mask_shape_not_found` — stub `masks_update` returning `DT_REMOTE_ERR_NOT_FOUND`; assert error fixture pair with `not_found`, `details.parameter == "id"`.
5. `test_set_mask_attachment_round_trip` and `test_set_mask_attachment_sum_error` — attach happy path + `sum_is_brush_only` error fixture.
6. `test_mask_methods_require_capability` — dispatch each of the five methods against a session whose hello did not advertise `mask_shapes`; assert refusal (mirror how existing per-capability gating is tested at the protocol layer — if gating is client-side only, this test asserts the sidecar layer instead in Task 9 and is omitted here).

- [ ] **Step 2: Author the fixtures**

Copy the JSON-RPC envelope framing byte-for-byte from `set_module_params_curve_request/response.json`. Contents:

`create_mask_shape_request.json` params:
```json
{ "type": "circle", "space": "preview",
  "geometry": { "center": [0.62, 0.41], "radius": 0.10, "border": 0.03 },
  "name": null, "attach": { "op": "exposure", "instance": 0 } }
```
`create_mask_shape_response.json` result:
```json
{ "shape": { "id": 100, "type": "circle", "name": "circle #1", "space": "preview",
    "editable": true,
    "geometry": { "center": [0.62, 0.41], "radius": 0.11, "border": 0.04, "size_mapping": "exact" },
    "raw_geometry": { "center": [0.598, 0.463], "radius": 0.093, "border": 0.05 },
    "used_by": [ { "op": "exposure", "instance": 0, "state": ["union"], "inverted": false, "opacity": 0.85 } ] },
  "revision": 42 }
```
`set_mask_attachment_request.json` params:
```json
{ "op": "exposure", "instance": 0, "shape_id": 100, "attached": true,
  "state": "union", "inverted": false, "opacity": 0.8, "expected_revision": 42 }
```
The error fixtures use the standard error envelope (copy shape from an existing `*_error_*_response.json`) with `code` = `not_found` / `invalid_value` and `details` = `{"parameter":"id"}` / `{"parameter":"state","constraint":"sum_is_brush_only"}`.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_protocol --output-on-failure 2>&1 | tail -15`
Expected: compile FAILURE (calls table has no `masks_*`) or test failures.

- [ ] **Step 4: Implement the protocol changes**

In `remote_protocol.h`, append to `dt_remote_protocol_calls_t`:

```c
  // Tier-3 drawn masks (mask_shapes capability). NULL-returning-on-omit is
  // NOT used here (these are actions, not attach points); a NULL pointer in
  // a test call table means the method is unavailable.
  gboolean (*masks_list)(JsonNode **out, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*masks_create)(dt_masks_type_t type, JsonObject *geom, const char *name,
                           const dt_remote_module_ref_t *attach_ref, const uint64_t *expected_revision,
                           JsonNode **entry_out, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*masks_update)(dt_mask_id_t id, JsonObject *geom, const char *name,
                           const uint64_t *expected_revision, JsonNode **entry_out,
                           int *affects_out, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*masks_delete)(dt_mask_id_t id, const uint64_t *expected_revision,
                           JsonArray **removed_from_out, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*masks_attachment)(const dt_remote_module_ref_t *ref, dt_mask_id_t shape_id,
                               gboolean attached, const char *state, int inverted,
                               const double *opacity, const uint64_t *expected_revision,
                               JsonNode **entry_out, uint64_t *revision, dt_remote_error_t **error);
```

Add `#include "develop/masks.h"` to `remote_protocol.h` for `dt_masks_type_t`/`dt_mask_id_t`.

In `remote_protocol.c`:
1. Both `DEFAULT_CALLS` and `s_calls` initializers gain `.masks_list = dt_remote_masks_list_json, .masks_create = dt_remote_masks_create_call, .masks_update = dt_remote_masks_update_call, .masks_delete = dt_remote_masks_delete_call, .masks_attachment = dt_remote_masks_attachment_call,`.
2. Hello: add `json_builder_add_string_value(b, "mask_shapes");` after `"quantity_params"` (and after M-B's `"parametric_mask_params"`/`"mask_render"`), with comment `// "mask_shapes" (mask Tier 3 / M-C) gates the drawn-mask methods.`
3. Add five handlers (`_handler_list_mask_shapes`, `_handler_create_mask_shape`, `_handler_update_mask_shape`, `_handler_delete_mask_shape`, `_handler_set_mask_attachment`), each: parse+validate params via the file's `_require_*`/`_optional_*` helpers, parse the geometry object (borrowed from the request tree), parse `expected_revision` (optional uint64), call the stubbed calls-table function, and serialize the result (`shape`/`shapes`/`removed_from`/`affects_instances` + `revision`) or `_handler_fail(err)`. Geometry parse: `type` string → `dt_remote_masks_type_from_string` at the handler boundary is NOT done here (the engine owns type mapping); pass the wire `type` string through to `masks_create` after mapping — map in the handler via `dt_remote_masks_type_from_string`, failing `unsupported_field`/`{parameter:"type"}` on an unknown/uneditable type before dispatch.
4. Add the five rows to `g_methods[]`:
```c
  { "list_mask_shapes",     TRUE,  FALSE, FALSE, _handler_list_mask_shapes },
  { "create_mask_shape",    TRUE,  TRUE,  FALSE, _handler_create_mask_shape },
  { "update_mask_shape",    TRUE,  TRUE,  FALSE, _handler_update_mask_shape },
  { "delete_mask_shape",    TRUE,  TRUE,  FALSE, _handler_delete_mask_shape },
  { "set_mask_attachment",  TRUE,  TRUE,  FALSE, _handler_set_mask_attachment },
```
   (Update the allowlist-count comment: "13 entries" → "18 entries".)

- [ ] **Step 5: Build and run C + Python protocol suites**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure`
Expected: all PASS.

Extend `tools/mcp/tests/test_protocol.py`'s shared-fixture parametrization with the five success pairs and the hello-capability assertion (`assert "mask_shapes" in client.capabilities`).

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_protocol.h src/control/remote_protocol.c \
        src/tests/unittests/control/test_remote_protocol.c \
        src/tests/unittests/control/fixtures/ tools/mcp/tests/test_protocol.py
git commit -m "feat: drawn-mask wire surface — mask_shapes capability + five methods

hello advertises mask_shapes; list/create/update/delete_mask_shape and
set_mask_attachment (attach/detach merged, H7) dispatch to the engine with
CAS + fixtures shared between the C and Python protocol suites."
```
(plus trailers)

---

### Task 9: Sidecar — five tools + capability gate + docstring token budget

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py` (append)

- [ ] **Step 1: Write the failing tests**

Append to `test_tools.py`, mirroring the quantity/blend capability-gate tests exactly. For each of the five tools:
- `test_<tool>_gated_on_mask_shapes_capability` — a fake server whose hello omits `mask_shapes`; assert the tool raises with `"mask_shapes"` and `"upgrade darktable"` in the message and the wire handler was not called.
- `test_<tool>_passes_through_when_advertised` — capability advertised; assert the params forwarded verbatim (e.g. `create_mask_shape` forwards `type`, `geometry`, `attach`).
Plus one budget test:
```python
def test_mask_tool_docstrings_within_h7_budget():
    from darktable_mcp import server as srv
    def words(fn): return len((fn.__doc__ or "").split())
    # H7: create+update combined <= 1500 tokens (~ words*1.3); others <= 500 tokens each.
    assert words(srv.create_mask_shape) + words(srv.update_mask_shape) <= 1150   # ~1500 tokens
    for fn in (srv.list_mask_shapes, srv.delete_mask_shape, srv.set_mask_attachment):
        assert words(fn) <= 385   # ~500 tokens
```
(Adjust the word→token ratio to match whatever the repo's other budget tests use, if any; otherwise this word-count proxy is the budget gate.)

- [ ] **Step 2: Run to verify failure**

Run: `cd tools/mcp && .venv/bin/pytest -q tests/test_tools.py -k mask`
Expected: FAIL — tools not defined.

- [ ] **Step 3: Implement**

Add five FastMCP tools to `server.py`, each gated client-side on `mask_shapes` exactly like the quantity/blend gate (`await client.ensure_connected(); if "mask_shapes" not in client.capabilities: raise TransportError(...)`). Docstrings within the H7 budget:
- `create_mask_shape(type, geometry, name=None, attach=None, space="preview", expected_revision=None)` and `update_mask_shape(id, geometry, name=None, space="preview", expected_revision=None)` — carry the full coordinate contract ONCE (in `create_mask_shape`), and `update_mask_shape` cross-references it ("coordinates as in create_mask_shape"). Both note: coordinates are preview-normalized `[0,1]`; sizes/angles may be reported `approximate` under perspective (`size_mapping`); editing a shared shape edits every module using it (`affects_instances`).
- `list_mask_shapes()`, `delete_mask_shape(id, expected_revision=None)`, `set_mask_attachment(op, shape_id, attached, instance=0, state=None, inverted=None, opacity=None, expected_revision=None)` — terse; `set_mask_attachment` notes `attached=true` upserts (state/inverted/opacity apply) and `attached=false` detaches (shape survives if used elsewhere). One sentence records the first-member rule: `state` is ignored when the shape becomes the bottom member of a module's group (that member has no combine op); `inverted`/`opacity` still apply.

- [ ] **Step 4: Run tests**

Run: `cd tools/mcp && .venv/bin/pytest -q`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/mcp/src/darktable_mcp/server.py tools/mcp/tests/test_tools.py
git commit -m "feat: sidecar drawn-mask tools (5) gated on mask_shapes, H7 docstring budget"
```
(plus trailers)

---

### Task 10: Integration gates (M-B `show_mask` as primary observable)

**Files:**
- Create: `tools/mcp/tests/integration/test_masks_tier3.py`

**Prereq:** a running build (`build/bin/darktable`). Run with:
`DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_masks_tier3.py`

- [ ] **Step 1: Write the gates**

Model on `test_quantity_milestone6.py`: module docstring, `from . import harness`, `pytestmark = pytest.mark.integration`, raw wire calls via `client.call(...)`, `harness.wait_for_stable_revision`. Use M-B's `render_preview` with `show_mask: {op, instance}` (grayscale JPEG, white = full effect) as the primary observable. Per H6: assert inside/outside delta **ratios** on the preview pipe at a pinned `max_px`, with a fixture image chosen for a high-contrast region. Gates (each its own test, definition order):

1. `test_hello_advertises_mask_shapes` — `client.capabilities` contains `"mask_shapes"`.
2. `test_create_circle_confines_effect` — enable exposure with a visible delta; `create_mask_shape {type:"circle", geometry:{center:[0.5,0.5],radius:0.15,border:0.02}, attach:{op:"exposure",instance:0}}`; render `show_mask {op:"exposure",instance:0}`; assert the mask is bright inside the circle centroid and dark in a far corner (ratio of mean luminance inside vs outside > 3×). Assert the response `used_by` shows exposure and `mask_mode` reads `drawn` via `get_module_params`' blend section.
3. `test_update_moves_region` — `update_mask_shape` moving the center to `[0.25,0.25]`; re-render `show_mask`; assert the bright centroid moved toward the new position (bright-mass centroid within tolerance of `[0.25,0.25]` in preview coords — the placement gate comparing requested preview coords to rendered mask centroid).
4. `test_distortion_roundtrip_and_freshness` — enable `crop` with a rotation in the SAME client breath as a `create_mask_shape` at preview position `P` (issue the crop mutation then immediately the create, relying on the freshness contract to block until the pipe reflects the rotation); then `list_mask_shapes` returns the shape's `geometry.center` within tolerance of `P` (the back-transform gate — the milestone's highest-value test). Also assert that if the pipe is forced busy the create returns a `retry_later` error rather than a mis-placed shape (best-effort; skip if the harness cannot force a dirty pipe deterministically). **Angle-space coverage note:** a *conformal* distortion (plain rotation) does not distinguish the isotropic-pixel angle probe (Amendment 5) from a normalized-space one — both round-trip. The isotropic handling only diverges under an **anisotropic** distortion (perspective/keystone). If a perspective-correction fixture is available, add an ellipse `rotation` round-trip under it asserting the reported wire angle matches the geometric major-axis angle in the rendered `show_mask`; otherwise record this as a known coverage gap (the implementation is still correct-by-construction per Amendment 5, but untested end-to-end).
5. `test_detach_makes_effect_uniform_then_delete_noops` — `set_mask_attachment {attached:false}`; render normal preview; assert the exposure effect is now spatially uniform (inside/outside ratio ≈ 1); `delete_mask_shape`; assert a re-render is unchanged and `list_mask_shapes` no longer contains the id.
6. `test_deferred_type_listed_non_editable` — using a fixture edit that already contains a brush (or skip with a clear reason if no such fixture exists), assert `list_mask_shapes` lists it with `editable == false` and no `geometry`, and that `set_mask_attachment` on it succeeds.
7. `test_undo_twice_restores_create_attach` — capture revision; `create_mask_shape`+attach (2 history items); `undo` twice with the tracked revisions; assert `list_mask_shapes` and the module's `mask_mode` return to the pre-call state.

- [ ] **Step 2: Run the new gates**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp/tests/integration/test_masks_tier3.py`
Expected: all PASS (or a clearly-reasoned skip for gate 6 if no brush fixture exists).

- [ ] **Step 3: Run the full integration suite**

Run: `DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp`
Expected: previous gates + new gates pass (1 known OpenCL skip).

- [ ] **Step 4: Commit**

```bash
git add tools/mcp/tests/integration/test_masks_tier3.py
git commit -m "test: Tier-3 drawn-mask integration gates (show_mask observable, H6 ratios)"
```
(plus trailers)

---

### Task 11: Documentation + divergence manifest + design amendments + shared-contract cross-reference

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md`
- Modify: `docs/superpowers/specs/2026-07-18-darktable-mcp-picker-and-sampling-candidates.md` (freshness contract cross-reference)
- Modify: `docs/superpowers/specs/2026-07-19-darktable-upstream-divergence-manifest.md` (created in M-A)
- Modify: `tools/mcp/README.md`
- Modify: `.superpowers/sdd/progress.md` (ledger)

- [ ] **Step 1: Protocol reference** — add a top-level "Drawn masks (`mask_shapes` capability)" section covering: the coordinate contract (preview-normalized, `space` field, `raw_geometry`, the probe algorithm and `size_mapping` with the 1% spread threshold and 0.05 angle arm from Amendment 5); all five method shapes (`list_mask_shapes`, `create_mask_shape`, `update_mask_shape`, `delete_mask_shape`, `set_mask_attachment`); the state vocabulary (`union/intersection/difference/exclusion`, sum brush-only); per-method history-item counts (create-unattached = 1; create+attach = 2; update = 1; delete = one global masks-history item plus one item per module base group retired by the cascade; set_mask_attachment = 1); the deferred-type visibility rules; the full error table incl. `not_found`, `retry_later`, `raster_unsupported`, `sum_is_brush_only`; the **pipe-freshness contract** (Amendment 1, verbatim) **including the interactive-cost note** — the first coordinate call after a distortion-changing edit blocks the GTK main thread (and thus a co-located user's UI) until the preview pipe reprocesses, bounded by `pixelpipe_synchronization_timeout`, else `retry_later`; and the **GUI-edit-session guard** (Amendment 2, incl. the mid-drag-on-deleted-form behavior). Note the undo-granularity wart (N calls to revert an N-item op).

- [ ] **Step 2: Supported operations** — add: "Drawn masks (circle, ellipse, gradient create/edit/attach) are supported via the `mask_shapes` capability; path/brush/clone/AI forms are visible and attachable but not editable; raster masks remain out of scope. `mask_manager` stays unsupported as a *module* — the drawn-mask methods replace it."

- [ ] **Step 3: Drawn-masks design-doc amendments** — set the status line to "planned (see plan 2026-07-19-darktable-mcp-drawn-masks-tier3.md)"; add an "Amendments (planning)" subsection recording all five amendments from this plan's header (freshness contract; GUI guard; H7 five-tool merge; require-all-members; probe constants), and resolving the design's "Explicitly unresolved" list: freshness contract (Amendment 1), probe constants (Amendment 5), omitted-member policy (Amendment 4), and noting `duplicate_mask_shape` remains deferred and the disabled-module-reveal question is answered as "reveal iff exactly one referencing module, regardless of enabled state".

- [ ] **Step 4: Picker/sampling candidates cross-reference** — in `2026-07-18-darktable-mcp-picker-and-sampling-candidates.md`'s "Open questions for the eventual design", mark the **Pipe-freshness contract** bullet as **answered**: "Answered by the drawn-masks Tier-3 plan (`2026-07-19-darktable-mcp-drawn-masks-tier3.md`, Design amendment 1): block-until-clean on `dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID`, enqueue one reprocess, poll 5 ms × `pixelpipe_synchronization_timeout` (cap 2000), else `retry_later`. `sample_region` inherits `dt_remote_transform_ensure_fresh` verbatim from `remote_transform.c`." Also update the coordinate-space bullet to point at the shared `remote_transform.c` helper.

- [ ] **Step 5: Divergence manifest** — append rows to `2026-07-19-darktable-upstream-divergence-manifest.md`:

| upstream file | divergence | guard |
|---|---|---|
| `src/develop/masks/masks.c` / `masks.h` | `_group_create` renamed+exported as `dt_masks_group_create_for_module` (3-arg, type retained) | linkage test `test_remote_masks` |
| `src/control/remote_transform.c` (NEW) | shared preview↔raw transform + freshness contract | `test_remote_transform` unit tests |
| `src/control/remote_masks.c` (NEW) | drawn-mask engine over `dev->forms`/group forms | `test_remote_masks` unit tests |

Plus a "rehearsed procedure: upstream changes a masks point struct / adds a shape type" paragraph: update `remote_masks.c`'s per-type (de)serializers + validation ranges + the `dt_masks_point_*` sizes, extend `dt_remote_masks_type_string`/`_from_string`, add a geometry round-trip test row.

- [ ] **Step 6: README** — add a "Drawn masks" subsection with the walkthrough: create circle on exposure → verify with `render_preview(show_mask=...)` → `update_mask_shape` to adjust → `set_mask_attachment(attached=false)` to detach. Include the `size_mapping`-approximate and shared-shape (`affects_instances`) warnings and the **manual test for the GUI guard** (Amendment 2): open the image in darkroom, start dragging a circle, issue a remote `delete_mask_shape` on it, confirm no crash and the drag ends cleanly.

- [ ] **Step 7: Full verification stack**

Run all four:
```bash
cmake --build build -j$(nproc)
ctest --test-dir build
cd tools/mcp && .venv/bin/pytest -q && cd ../..
DARKTABLE_BIN=$PWD/build/bin/darktable tools/mcp/.venv/bin/pytest -q -m integration tools/mcp
```
Expected: all green.

- [ ] **Step 8: Ledger + commit**

Append one line to `.superpowers/sdd/progress.md` recording the milestone (task list, final commit, "full stack verified"). Then:

```bash
git add docs/ tools/mcp/README.md .superpowers/sdd/progress.md
git commit -m "docs: drawn masks (Tier 3 / M-C) — protocol reference, manifest, amendments

Adds the mask_shapes protocol section, records the pipe-freshness contract
(answered here, shared with sample_region) and the GUI-edit guard, the H7
five-tool merge, and the divergence-manifest rows."
```
(plus trailers)

---

## Self-review notes (performed while writing)

- **Spec coverage:** coordinate contract → Task 2 (transform) + Task 3 (serializers); `list_mask_shapes` → Task 4; create/update/delete → Task 5; attach/detach (merged) → Task 6; engine CAS/reveal/revision → Task 7; wire + capability + fixtures → Task 8; sidecar + docstring budget → Task 9; integration (`show_mask`, placement, distortion round-trip, undo, deferred type) → Task 10; docs/manifest/design-status/picker-cross-ref → Task 11. The two PLANNING PREREQUISITES (freshness contract, GUI guard) are written as Design amendments 1–2 with full normative text and each has a test (Task 2 unit + Task 10 gate 4; Task 5 unit + Task 11 manual). H7 → Amendment 3 (five tools) + Task 9 budget test. H6 → Task 10 ratio assertions at pinned size. Design decisions 1–8 all land (decision 8 = Amendment 2; decision 4 = groups-not-listed in Task 4; decision 5 = affects_instances in Task 5/7; decision 7 = opacity/inverted on set_mask_attachment in Task 6).
- **Design deviations recorded as amendments:** attach/detach merged into `set_mask_attachment` (H7); require-all-members (open question resolved); probe constants pinned; freshness = block-until-clean + retry_later. All in the amendments section and written back to the design doc in Task 11.
- **Verified source facts, not contradictions:** remote creation uses
  `dt_masks_gui_form_save_creation_ext` for ordered forced-new snapshots,
  and remote full deletion uses `dt_masks_form_remove_shape_full`;
  compatibility behavior remains unchanged for legacy callers.
- **Type consistency:** `dt_remote_masks_geometry_to_points(dev, type, JsonObject*, void*, error**)`, `dt_remote_masks_list(dev)`, `dt_remote_masks_set_attachment(...)`, and the `dt_remote_masks_*_call` entry points keep identical signatures across the task where they are declared (header), implemented, and consumed (Tasks 7–8). Error codes `DT_REMOTE_ERR_NOT_FOUND`/`DT_REMOTE_ERR_PIPE_NOT_READY` are declared once (Task 2) and mapped once (Task 7). Wire slugs `not_found`/`retry_later` match the Global Constraints table.
- **Placeholder scan:** the deliberate `ensure_fresh` reminder is
  immediately followed by its corrected final form. All other steps carry
  complete code or the M-A-style "mirror the neighbouring test's
  plumbing" instruction.
- **Implementer guidance:** stub-table/test-helper names in Tasks 8–9 must match the existing local idioms (`_assert_dispatch_matches`, `dt_remote_protocol_set_calls`, `fake_server_factory`); confirm `dt_remote_error_t`'s details field name (`details_json`) against M-A's actual struct before relying on it. Legacy `(module, grp, form)` detach remains hardwired to `darktable.develop`; Task 5 remote full deletion instead uses `dt_masks_form_remove_shape_full(dev, form, &references)`.
