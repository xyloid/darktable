# darktable MCP rgbcurve Technical Debt Register

- **Status:** Open
- **Recorded:** 2026-07-11
- **Audited tree:** `24aae7fab4c105cfd99f300b5a6cb982e27af392`
- **Origin:** Review of `d709834d2c` (`remote_curve: rgbcurve adapter
  callbacks and atomic apply path`) and the subsequent Milestone 2
  reconciliation.

## Purpose

Milestone 2 is feature-complete: semantic curve requests reach the atomic
mutation engine, curve capabilities are active, the MCP sidecar exposes curve
editing, and live integration coverage exists. This register preserves the
remaining review debt separately from the completed milestone plans so that
the completion markers are not mistaken for a zero-debt assessment.

The production curve engine and its rgbcurve callbacks are unchanged between
`d709834d2c` and the audited tree. The findings below therefore remain open
unless a later commit explicitly closes them with the listed regression
coverage and a fresh review.

## Debt summary

| ID | Severity | Status | Finding |
|---|---|---|---|
| TD-RGB-001 | Important | Open | Scalar-only prepare-field mutations are blocked by semantic registry drift. |
| TD-RGB-002 | Important | Open | Adapter preparation can mask semantic ID, class, or duplicate-request errors. |
| TD-RGB-003 | Minor | Open | Registry validation accepts native widths that rgbcurve transition callbacks do not safely access. |
| TD-RGB-004 | Important (coverage) | Partially mitigated | The C transaction inventory uses a synthetic transaction instead of the production mutation entry point. |

## TD-RGB-001: Preserve primitive prepare-field mutation under registry drift

### Current behavior

`dt_remote_curve_apply_patch()` computes `prepare_needed` for either a semantic
patch or a scalar patch mentioning `curve_autoscale` or
`compensate_middle_grey`. It then unconditionally validates the semantic
adapter before allowing the scalar transaction to proceed:

- `src/control/remote_curve.c:801-806`

Consequently, registry drift correctly disables semantic mutation, but it also
blocks scalar-only writes to those two primitive fields. This conflicts with
the standing failure policy that registry drift disables semantic support
without disabling primitive mutation.

### Impact

A client can continue editing unrelated primitive fields after semantic
registry drift, but cannot edit two otherwise valid primitive rgbcurve fields.
The behavior depends on whether a primitive field happens to be listed in the
adapter's `prepare_fields`, which leaks an internal semantic implementation
detail into the primitive API.

### Required resolution

- If the request contains semantic values, registry validation failure remains
  `DT_REMOTE_ERR_INTERNAL` and the entire transaction remains unchanged.
- If the request is scalar-only and registry validation fails, keep the scalar
  mutation available and skip semantic adapter preparation/validation for that
  request.
- When registry validation succeeds, retain the existing rgbcurve transition
  behavior for prepare fields.
- Document that skipping the transition under actual registry drift is a
  deliberate degraded mode: primitive availability takes precedence because
  an unvalidated semantic adapter must not touch the native curve layout.

### Closure tests

- With one synthetic introspection drift, a semantic patch returns `internal`
  and changes no params, enable state, history, GUI state, or revision.
- Under the same drift, a scalar-only write to a prepare field succeeds and is
  read back as the requested primitive value.
- The drifted scalar-only path does not invoke adapter `prepare()` or
  `validate_completed()`.
- Schema and semantic-value reads remain fail-closed under the same drift.

## TD-RGB-002: Run structural request preflight before adapter preparation

### Current behavior

The adapter's `prepare()` callback runs before the loop that rejects null
semantic entries, wrong classes, unknown semantic IDs, and duplicates:

- preparation: `src/control/remote_curve.c:811-813`
- structural preflight: `src/control/remote_curve.c:815-856`

For example, a request that changes middle-grey compensation without an
available work profile and also names an unknown semantic ID can return
`unsupported_field/work_profile_unavailable` instead of the required
`unknown_field`. Equivalent masking is possible for wrong-class and duplicate
errors.

### Impact

The reported error depends on live adapter state rather than the request's
structural validity. This violates structural error precedence for clients and
performs avoidable transition work for a request that should have been rejected
without consulting the live profile.

### Required resolution

Reorder the nonmutating stages of `dt_remote_curve_apply_patch()`:

1. Resolve and validate every semantic entry's presence, class, ID, and
   uniqueness against the compiled adapter.
2. Validate the adapter/introspection pair when semantic support is needed.
3. Run adapter `prepare()`.
4. Evaluate predicates and validate/write curves against the post-transition
   projected params block.
5. Run completed-state validation.

Predicate checks, omitted-interpolation resolution, curve validation, and
writes must remain after preparation because they depend on the projected
post-transition state.

### Closure tests

- Unknown ID plus missing work profile returns `DT_REMOTE_ERR_UNKNOWN_FIELD`.
- Wrong class plus missing work profile returns
  `DT_REMOTE_ERR_UNSUPPORTED_FIELD` for the class violation.
- Duplicate semantic ID plus missing work profile returns
  `DT_REMOTE_ERR_INVALID_VALUE` with `duplicate_parameter`.
- In every case, `prepare()` is not called, no native-curve mutation occurs,
  the already-projected temporary block is discarded, and live state remains
  unchanged.
- A valid compensation request with no work profile still returns
  `unsupported_field` with `constraint: "work_profile_unavailable"`.

## TD-RGB-003: Align validated native widths with callback accessors

### Current behavior

Registry validation accepts all supported integer/enum widths for count, type,
and internal-version leaves, and accepts either `float` or `double` node
coordinates:

- integer/enum acceptance: `src/control/remote_curve_registry.c:735-752`
- floating acceptance: `src/control/remote_curve_registry.c:754-756`
- descriptor checks: `src/control/remote_curve_registry.c:794-831`

The rgbcurve transition helpers subsequently require or hard-cast exact
`int`/`float` storage:

- enum read: `src/control/remote_curve_registry.c:341-355`
- node access: `src/control/remote_curve_registry.c:377-398`
- count/type copy: `src/control/remote_curve_registry.c:463-486`
- compensation transform: `src/control/remote_curve_registry.c:490-520`

An adapter layout can therefore be accepted and cached as valid, then fail only
when a transition callback executes.

### Impact

The validation cache does not guarantee that all adapter callbacks can safely
access the layout it approved. The current rgbcurve layout happens to use
`int` and `float`, but the mismatch becomes more important when Milestone 3
extracts these helpers for additional adapters.

### Required resolution

Choose one contract and make validation and execution agree:

- Preferred: reuse or extract width-aware integer and floating accessors for
  the transition callbacks, preserving the registry's generic accepted-shape
  contract.
- Acceptable alternative: add adapter-specific validation that rejects every
  layout not matching the callbacks' exact `int`/`float` assumptions.

Whichever approach is chosen, a layout cached as valid must not later fail
solely because a callback uses a narrower accessor contract.

### Closure tests

- A synthetic non-default-width layout either passes validation and completes
  the relevant transition safely, or is rejected during registry validation.
- Repeated validation uses the cached result and produces the same outcome.
- The real rgbcurve introspection and all existing transition tests remain
  green.

## TD-RGB-004: Replace or formally supplement the synthetic transaction seam

### Current coverage

The named C transaction tests call `apply_scratch_transaction()` in
`src/tests/unittests/control/test_remote_edit.c:907-937`. That helper manually
copies params, invokes pure helpers, commits with `memcpy`, updates a synthetic
enable flag, and increments a synthetic history counter. It does not call
`dt_remote_set_module_params()` and cannot detect regressions in the real CAS,
GUI synchronization, history, revision-tracker, or result-wiring path.

Later live integration coverage in
`tools/mcp/tests/integration/test_curves.py` partially mitigates this finding:

- lines 104-140: real CAS mutation, semantic read-back, revision advance, and
  visible preview change;
- lines 143-175: scalar + curve + enable produces one history item;
- lines 178-218: one undo restores prior points and mode;
- lines 221-304: projected-mode transition and rejected linked-mode write;
- lines 307-336: stale revision leaves scalar/semantic state and revision
  unchanged.

The history test sends `enable: True` while rgbcurve is already enabled, so it
does not prove that a real enabled-state transition participates in the same
transaction. The live suite also does not explicitly pin one GUI
synchronization, and it does not exercise invalid-curve and missing-profile
failures through the complete live transaction while checking every observable
for nonmutation. No recorded re-review accepted the live suite as full closure
of the original finding.

### Required resolution

Use one of these closure paths:

1. Add one stable direct C fixture that calls `dt_remote_set_module_params()`
   against a live module/develop transaction and can count GUI, history, and
   revision effects; or
2. Expand the live integration suite to cover the missing failure invariants,
   an actual enabled-state transition, one history item, and the correct
   visible preview/GUI result; retain focused pure tests for callback-only edge
   cases and obtain a formal review accepting that combination as
   live-equivalent coverage. If an exact GUI-sync count remains a closure
   requirement, add an instrumented C seam for that count even when choosing
   this path.

Do not add another helper that manually reproduces the production commit
sequence; the purpose is to make regressions in the real wiring observable.

### Closure tests

- One successful request combines scalar values, multiple curves, an actual
  enabled-state transition, and `expected_revision`; it produces one
  transaction commit, one history item, the expected revision transition,
  semantic live read-back, and one-undo restoration. An instrumented C path
  must observe exactly one GUI sync; an accepted live-equivalent path must
  instead prove the correct visible preview/GUI state.
- Invalid curve, missing profile, and stale revision attempts each leave params,
  enable state, history, GUI state, and revision unchanged.
- At least one test fails if the production call to
  `dt_remote_curve_apply_patch()` is removed or moved after the live commit.

## Related housekeeping debt

These items are not product defects, but resolving them will keep future work
from relying on stale information:

- `src/control/remote_edit.h:243-249` still says nothing constructs or reads
  `semantic_values`; the protocol and mutation engine now do both.
- `src/control/remote_edit.h:349-355` still describes semantic application as a
  future extension even though curves are implemented.
- `.superpowers/sdd/progress.md` records the review-fixes run only through Task
  4, while the committed plans mark Tasks 5-6 and Milestone 2 complete.
- `stash@{0}` (`9fcedb843c...`) and `stash@{1}` (`b3dd780816...`) are historical
  pre-Task-8/Task-7 safety snapshots. Their functional changes are superseded;
  do not apply them to the current tree. Inspect and deliberately drop them
  only after no recovery value remains.

## Recommended resolution order

1. Resolve TD-RGB-001 and TD-RGB-002 together because both change the ordering
   and failure policy in `dt_remote_curve_apply_patch()`.
2. Resolve TD-RGB-003 before Milestone 3 Task 1 extracts shared adapter helpers;
   otherwise the width-contract mismatch will be generalized into new code.
3. Close TD-RGB-004, run the complete verification matrix, and request a fresh
   review of the mutation engine plus its real transaction coverage.
4. Clean up the related comments, ledger, and obsolete stashes.

Milestone 3 implementation should not begin until TD-RGB-001 through
TD-RGB-003 are resolved or explicitly waived. Its plan assumes the Milestone 2
engine is stable and forbids engine changes, while TD-RGB-001 and TD-RGB-002
require such changes.

## Verification matrix for closure

```bash
cmake --build build --target \
  test_remote_curve \
  test_remote_curve_registry \
  test_remote_edit \
  test_remote_protocol

ctest --test-dir build \
  -R 'test_remote_(curve|curve_registry|edit|protocol)' \
  --output-on-failure

ctest --test-dir build --output-on-failure

cd tools/mcp
.venv/bin/pytest -q
DARKTABLE_BIN=$PWD/../../build/bin/darktable \
  .venv/bin/pytest -m integration -q
```

## Closure protocol

For each debt item:

- add a regression test that fails for the recorded behavior;
- implement the smallest production change that satisfies the stated contract;
- run the relevant focused tests and the full matrix above;
- obtain code review of both the production fix and the load-bearing tests;
- update this file's status table with the closing commit and review result.
