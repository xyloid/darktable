# darktable MCP drawn-mask Task 5 remediation design

Date: 2026-07-24
Status: approved for implementation planning
Companions:

- `docs/superpowers/specs/2026-07-19-darktable-mcp-drawn-masks-design.md`
- `docs/superpowers/plans/2026-07-19-darktable-mcp-drawn-masks-tier3.md`
- Task 5 implementation commit `635ba962af`

## Problem

Task 5 implements mask-shape creation, update, deletion, and the live GUI
edit-session guard. Its direct-group cases pass, but review found that it
assumes module mask groups are flat. darktable supports nested groups,
including groups produced by AI object masks. A full delete of a nested
leaf can consequently leave its ID in a parent group after the form has
been removed from `dev->forms`. The group renderer is not safe against
that dangling reference.

The same review found four related defects:

1. The GUI guard checks only immediate membership and is not called before
   create-with-attachment mutates an existing group.
2. Headless create-with-attachment records history with `enable = TRUE`,
   enabling a module that was disabled before the request.
3. `DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK` is applied before both
   creation snapshots, although the Tier-3 design requires it only in the
   attachment snapshot.
4. Permanently removed forms and emptied groups are unlinked from
   `dev->forms` without being freed or transferred to `dev->allforms`.

The Task 5 tests set `dev->gui_attached = FALSE`, so they do not exercise
the production masks-history path and cannot detect the two history
defects.

## Goals

- Make transitive group membership a single, cycle-safe core concept.
- Give the remote API an ownership-safe full-delete operation for
  non-group mask forms.
- Preserve the public behavior of legacy `dt_masks_form_remove` until its
  callers have been audited individually.
- Preserve a module's enabled state during remote mask creation and
  deletion.
- Make the two create-with-attachment history states coherent and match
  the Tier-3 design.
- Cancel live GUI editing for transitive targets and before adding a form
  to a currently edited group.
- Leave a durable, grep-friendly audit trail for later review and
  migration of legacy removal callers.

## Non-goals

- Do not change the wire protocol or error vocabulary.
- Do not expose group forms as wire objects.
- Do not permit remote deletion of group forms.
- Do not change legacy `dt_masks_form_remove` behavior in this
  remediation.
- Do not migrate unrelated GUI, mask-manager, module-reset, spots, or
  retouch callers.
- Do not batch create-with-attachment into one undo item.
- Do not repair arbitrary pre-existing dangling group IDs during an
  otherwise unrelated mutation.

## Chosen approach

Add explicit core graph, owner-discovery, creation, and full-delete APIs.
Task 5 migrates to the new APIs. The existing
`dt_masks_form_remove(module, grp, form)` remains available with its
current semantics, and its callers are recorded in a checked-in audit
ledger for later review.

This avoids silently changing history, GUI updates, or ownership behavior
for the many legacy call patterns while establishing safe primitives for
new remote code and future migrations.

## Core graph interfaces

Add these declarations to `src/develop/masks.h` and implementations to
`src/develop/masks/masks.c`:

```c
gboolean dt_masks_group_contains_form(const dt_develop_t *dev,
                                      const dt_masks_form_t *group,
                                      dt_mask_id_t formid);

GPtrArray *dt_masks_form_get_referencing_modules(const dt_develop_t *dev,
                                                 dt_mask_id_t formid);

gboolean dt_masks_form_remove_shape_full(dt_develop_t *dev,
                                         dt_masks_form_t *form,
                                         GPtrArray **affected_modules);
```

`dt_masks_group_contains_form` returns true when `group` has `formid`
itself or contains it at any depth. It returns false for null inputs,
non-group roots, missing children, and absent targets. It uses a visited
ID set so malformed legacy cycles terminate without recursion loops.

`dt_masks_form_get_referencing_modules` examines every blending module's
base mask group using the containment query. It returns a newly allocated
`GPtrArray`; its entries are borrowed `dt_iop_module_t *` pointers and the
caller releases only the array. Each module appears at most once,
regardless of duplicate or multiple transitive paths.

`dt_masks_form_remove_shape_full` accepts only a non-group form that is
present in `dev->forms`. It returns false without mutation for invalid
arguments. On success, it optionally returns the pre-mutation referencing
module array through `affected_modules`; when that output is null, the
function releases the array itself.

## Full-delete algorithm and ownership

Full deletion performs these steps in order:

1. Collect the transitively referencing modules.
2. Remove every direct membership whose `formid` is the target ID from
   every group in `dev->forms`.
3. Queue only parent groups that became empty because of step 2.
4. Repeatedly process each queued empty group while leaving the target
   and every queued/cascaded group active in `dev->forms`. For every
   module whose `mask_id` names that group, clear its ID and
   `DEVELOP_MASK_MASK`, preserve all other mode bits and enabled state,
   and immediately record that module's forced-new history snapshot.
5. Remove the empty group's ID from all remaining groups and queue newly
   emptied parents, still without retiring any queued form.
6. Once the queue drains and every affected module snapshot has been
   recorded, retire the target and all queued groups.
7. Record the final global mask-manager snapshot of the pruned graph.

Retiring a form means removing its pointer from `dev->forms` and appending
it to `dev->allforms` exactly once. Forms are not freed immediately:
history and pixel-pipe snapshots require develop-lifetime ownership.
Duplicate incoming memberships are all removed. Unrelated empty groups
that predate the request are left unchanged.

The operation records one global masks-history item for the shape
deletion and one module masks-history item for each unique module whose
base group is retired by the cascade. Module history uses the module's
current `enabled` value rather than forcing it true. Thus every replay
prefix is coherent: a still-valid module `mask_id` always resolves to an
active form, then the final global snapshot retires the graph. This
preserves the Tier-3 contract of `1 + affected modules` history items.

## Creation interface and history

Add an extended creation interface while retaining
`dt_masks_gui_form_save_creation` as a compatibility wrapper:

```c
typedef struct dt_masks_form_creation_options_t
{
  const char *requested_name;
  gboolean preserve_module_enabled;
  uint32_t mask_mode_to_add;
} dt_masks_form_creation_options_t;

void dt_masks_gui_form_save_creation_ext(
  dt_develop_t *dev,
  dt_iop_module_t *module,
  dt_masks_form_t *form,
  dt_masks_form_gui_t *gui,
  const dt_masks_form_creation_options_t *options);
```

The compatibility wrapper supplies default naming, the legacy
history-enable behavior, and `mask_mode_to_add = 0`. Its existing callers
therefore retain their current sequence.

The remote caller supplies:

```c
{
  .requested_name = name_or_null,
  .preserve_module_enabled = TRUE,
  .mask_mode_to_add = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
}
```

The extended helper assigns a unique ID and either a unique requested
name or the shape's default name, then appends the form and records the
first history item. For an attached creation it next finds or creates the
module group, adds the membership, ORs `mask_mode_to_add`, and records the
second history item. When `preserve_module_enabled` is true, both history
calls pass the module's existing enabled value instead of `TRUE`.

Each forced-new masks history call is also a production undo boundary:
it bypasses edited-target suppression and encloses its signal-backed
`DT_UNDO_HISTORY` record in a first-class isolated undo scope. The scope
holds the recursive undo mutex from begin through record and end, and
stamps the committed group between two coalescing epochs, so ordinary
records cannot time-coalesce across either side. If an ordinary group is
active, the scope closes its nonempty prefix and lazily resumes a suffix
only on the next accepted record. Gated or disabled records therefore
leave no empty group, stale one-shot state, or redo invalidation. Ordinary
masks-history calls retain their existing target/time merging behavior.

Public undo or redo can also run while a long-lived ordinary group remains
logically open. Before traversal, the undo core discards an empty physical
segment or closes a nonempty one, clears its active-marker alias, and leaves
the group depth intact. A later accepted record opens a new segment lazily;
the group mutex is not retained across the background operation. Saved
ambient markers are treated defensively if they are no longer in the undo
list. `dt_undo_disable_next` is likewise a one-shot token for the thread
that armed it: interleaved records from other threads do not consume it,
and an explicit clear cancels it.

In-memory mask history items explicitly distinguish “no forms snapshot”
from an explicit empty forms snapshot via `forms_history`. The
`masks_history` table has no row for an empty list, so reload infers that
state for the final `mask_manager` snapshot used by remote full deletion;
the staged non-manager snapshots retain active forms and persist as
ordinary mask rows. Reloading an arbitrary empty snapshot on a
non-`mask_manager` item remains a pre-existing schema limitation and is
outside this remediation.

The resulting undo states are:

1. After the remote call: form present, membership present, group and
   `mask_id` valid, and `ENABLED | MASK` added to the prior mask mode.
2. After one undo: form present but unattached; module group, `mask_id`,
   mask mode, and enabled state match their pre-call values.
3. After two undos: the complete pre-call state, including absence of the
   newly created form.

Making `requested_name` explicit removes the Task 5 implementation's
implicit use of a pre-populated `form->name` and avoids changing generic
GUI naming behavior.

## Remote integration

`src/control/remote_masks.c` uses the core graph query for all transitive
membership decisions:

- list response `used_by`;
- update response `affects_instances`;
- delete response `removed_from`;
- full deletion;
- live GUI edit-session targeting.

The GUI guard cancels when the visible form is the target, when
`form_gui->formid` is the target, or when the visible group transitively
contains the target. Before create-with-attachment mutates an existing
module group, the caller invokes the guard with that group.

Task 6 attachment work must reuse these core queries rather than add
another membership walk.

## Legacy caller audit trail

Implementation creates:

`docs/superpowers/specs/2026-07-24-darktable-mask-removal-caller-audit.md`

The audit contains one row for every `dt_masks_form_remove` call site,
including:

- source file and line anchor;
- arguments used (`module`, `grp`, and `form`);
- intended detach, group removal, or permanent-delete behavior;
- whether nested membership can reach the call;
- module-enabled and history assumptions;
- ownership behavior for unlinked forms;
- recommended migration or reason to retain the legacy call;
- review status.

The `dt_masks_form_remove` declaration and implementation receive a
stable `MASKS_REMOVE_CALLER_AUDIT` comment naming the audit path. This
gives future reviewers a source-level search marker without scattering
behavior-changing edits across legacy callers.

The Tier-3 design and plan are corrected to state that:

- legacy `dt_masks_form_remove` is shallow for ordinary full deletion;
- the new API supplies transitive full deletion for remote shapes;
- H2 membership is transitive;
- create-with-attachment applies mask mode only in the second snapshot;
- remote history preserves module enabled state.

The upstream divergence manifest records the new core APIs, the extended
creation helper, and the requested-name behavior.

## Error and malformed-state behavior

- A null develop or form input fails without mutation.
- A group passed to `dt_masks_form_remove_shape_full` fails without
  mutation; the remote layer continues to return its existing group-ID
  error.
- A missing child terminates that traversal branch.
- A cycle terminates at the first previously visited group ID.
- A target referenced more than once is removed from every parent slot,
  while each module appears once in result reporting.
- Argument validation and owner discovery finish before mutation starts.
  Once mutation begins, the operation has no recoverable error path and
  completes the ownership transfer and history sequence.

## Test design

Core graph tests cover direct membership, two-level nesting, a shared
inner group, duplicate paths, a missing child, and a manually constructed
cycle.

Full-delete tests cover:

- a nested leaf whose sibling survives;
- a sole nested leaf that empties and retires its ancestors;
- one inner group shared by two modules;
- exact `removed_from` owners;
- `mask_id` and only the `MASK` bit cleared when base groups disappear;
- every retired object present exactly once in `allforms`;
- teardown without manual freeing or double-free compensation;
- exact history count of one plus the number of retired module base
  groups.

GUI guard tests cover a visible outer group containing an inner group and
target leaf, an unrelated visible group, and creation into an existing
visible module group.

Creation tests run with history enabled and cover:

- disabled module remains disabled;
- enabled module remains enabled;
- no pre-existing group;
- a pre-existing group;
- the state after each of two undo operations;
- exact two-item history and revision deltas;
- requested-name preservation and duplicate UTF-8 suffixing;
- unchanged default naming through the compatibility wrapper.

All existing focused unit tests and the complete configured CTest suite
must remain green. `git diff --check` and a clean worktree check complete
verification.

## Acceptance criteria

- No successful remote full delete leaves the deleted ID in any group.
- All remote ownership and reporting decisions are transitive.
- Traversal terminates for missing references and malformed cycles.
- Removed objects remain owned until develop teardown.
- Create-with-attachment never changes module enabled state.
- Each create undo state is internally coherent and matches the Tier-3
  history contract.
- A live GUI edit is cancelled before every targeted remote mutation.
- The legacy caller inventory and migration recommendations are
  reviewable without reconstructing them from git history.
- The focused and full test suites pass.
