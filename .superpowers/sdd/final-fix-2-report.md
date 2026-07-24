# Final fix wave 2 report

## Scope

This wave starts from rejected implementation commit `711a027967` and
addresses the remaining undo-boundary and full-delete history-prefix
defects:

1. forced-new mask history must be a hard, two-sided isolation boundary,
   including while an ambient undo group is active;
2. full deletion must expose coherent target and cascaded-module prefixes
   at every public undo/redo boundary;
3. the public caveat and divergence/design documentation must match the
   implementation.

No schema change or migration of legacy deletion callers was in scope.

## RED evidence

The new public-path regressions initially produced 60 passing and 3
failing focused tests:

- a forced-new remote creation record followed immediately by an ordinary
  history record collapsed three expected boundaries into one;
- the same mutation inside an active outer group collapsed four expected
  boundaries into one;
- the first undo after a two-module full deletion still left the target
  absent instead of restoring the last staged prefix.

These failures exercised `dt_remote_undo`, not private undo-list
manipulation.

## Implementation

### Hard isolated undo scope

`src/common/undo.h` and `src/common/undo.c` now provide:

- a paired recording guard that retains the recursive undo mutex across
  develop history before/after capture;
- a first-class lazy isolated group that retains that mutex from begin
  through record and end;
- suspension of an ambient group, removal of an empty ambient marker, or
  closure of a nonempty ambient prefix before the isolated entry;
- lazy ambient-group resumption after the isolated entry;
- coalescing epochs on both sides of the isolated group, preventing a
  rapid ordinary record from merging into either neighbor;
- no empty marker or stale state when the isolated scope records nothing,
  including target-suppressed records;
- release-safe handling of undo operations invoked recursively from the
  same thread while the isolated scope is open.

Ordinary time coalescing, redo preservation for empty isolated scopes, and
existing non-isolated grouping behavior remain covered by focused tests.

`src/develop/develop.c` uses the isolated scope for forced-new mask
history and the recording guard for ordinary history. It acquires the
undo scope before `history_mutex`, captures the old snapshot while the
history lock is held, mutates history, captures the new snapshot, then
releases the undo scope. This preserves snapshot integrity and a single
lock order.

### Coherent full-delete prefixes

`dt_masks_form_remove_shape_full` now:

- keeps the target and newly empty cascaded groups alive until all
  per-module snapshots have been staged;
- clears each unique affected module and immediately records its
  forced-new snapshot while the corresponding empty group still exists;
- drains graph propagation before retiring the deferred forms;
- records the final global snapshot only after retirement.

Consequently every public undo boundary restores a complete graph/module
prefix, and redo traverses the same prefixes in the opposite direction.

### Empty mask snapshots

Develop history items now carry an in-memory `forms_history` sentinel so
an explicit empty mask snapshot is distinguishable from “this item has no
mask snapshot.” Replay and cleanup honor that distinction. On reload, the
final empty snapshot used by remote full deletion is inferred from the
persisted `mask_manager` history item.

### Documentation

The remote API caveat now states that remote creation and full deletion
use the dev-parameterized extended APIs; it no longer attributes those
paths to legacy `dt_masks_form_remove`. The remediation design/plan,
drawn-mask design, tier-3 plan, and upstream divergence manifest now
describe the hard isolated scope, staged deletion order, empty-snapshot
sentinel, and persistence limit.

## Regression coverage

`src/tests/unittests/control/test_remote_masks.c` now covers:

- an empty or suppressed isolated scope leaving no marker or stale state;
- mutex ownership from isolated begin through record and end;
- two-sided boundaries around an isolated record;
- rapid ordinary coalescing outside isolated scopes;
- redo preservation when an isolated scope records nothing;
- isolated mutation inside both empty and nonempty ambient groups;
- remote creation followed immediately by newer ordinary history;
- remote creation while an outer group remains active;
- exact public undo and direct redo traversal for a two-module cascading
  full deletion, including IDs, modes, enabled state, active empty groups,
  and the final empty graph.

## Review findings resolved

Independent review identified and this wave corrected:

- a potential ABBA order between the undo mutex and develop
  `history_mutex`;
- release-build handling of same-thread `clear` during an open isolated
  scope;
- documentation that could otherwise overstate general empty-snapshot
  persistence.

No further correctness issue was found in the isolated-state machine or
the staged remote full-delete path.

## Verification

Fresh verification in
`<REPO>/.claude/worktrees/mask-support`:

```text
cmake --build build --target test_remote_masks -j2
ctest --test-dir build -R '^test_remote_masks$' --output-on-failure
```

Result: focused binary 65/65 tests passed; CTest 1/1 passed.

```text
cmake --build build -j2
```

Result: build completed successfully through 100%.

```text
ctest --test-dir build --output-on-failure
```

Result: 19/19 tests passed, 0 failed (3.17 seconds).

`git diff --check` also completed with no whitespace errors.

## Known residual boundary

The existing `masks_history` schema cannot encode an explicit empty mask
snapshot attached to an arbitrary non-`mask_manager` history item because
an empty snapshot has no row. This is a pre-existing limitation of general
legacy history persistence and is outside this remediation wave.

It does not leave the remote full-delete path incomplete: each staged
non-manager snapshot retains active forms and persists as ordinary mask
rows, while the final empty snapshot is the `mask_manager` item that reload
logic recognizes explicitly.
