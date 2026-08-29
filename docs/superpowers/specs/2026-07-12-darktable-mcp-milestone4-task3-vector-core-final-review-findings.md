# darktable MCP Milestone 4 Task 3 Vector Core Final Whole-Range Review Findings

- **Status:** Open — request changes before merge
- **Reviewed:** 2026-07-12
- **Reviewed range:** `a29b8a2e76..a045d39e8c42e167240cc88df836965d89fd6050`
- **Final reviewer verdict:** **Ready to merge? No**
- **Follow-up scope:** The user requested findings documentation only. No
  production or test remediation was implemented in this follow-up.

## Relationship to the earlier review record

The earlier
`docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review.md`
records VEC3-001 through VEC3-010 and their historical remediation. Those ten
items remain historically remediated: this review does not reopen or rewrite
their closure evidence.

The later review examined the complete Task 3 range through exact head
`a045d39e8c42e167240cc88df836965d89fd6050` and found five additional issues.
Its whole-range result therefore supersedes the earlier record's overall
**Ready to merge: Yes** conclusion. The earlier record remains the audit trail
for VEC3-001 through VEC3-010; this document is the open findings record for
VEC3-011 through VEC3-015.

## Review summary

| ID | Severity | Status | Finding |
|---|---|---|---|
| VEC3-011 | Important | Open | `prepare_fields` can be traversed before registry validation. |
| VEC3-012 | Important | Open | Native aggregate array byte size is not validated. |
| VEC3-013 | Important | Open | Finite doubles outside the native float range can reach narrowing. |
| VEC3-014 | Important | Open | LEVELS ordering or minimum gap can collapse after float narrowing. |
| VEC3-015 | Minor | Open | A condition-ownership comment is stale. |

## VEC3-011: Validate before traversing `prepare_fields`

### Evidence and root cause

Evidence: `src/control/remote_vector.c:259,368,373`.

`patch_mentions_prepare_field()` dereferences
`adapter->prepare_fields[j]`. `dt_remote_vector_apply_patch()` calls that
helper to compute `prepare_needed` before registry validation has established
the adapter pointer/count invariant. The traversal can therefore run when
`prepare_field_count > 0` but `prepare_fields == NULL`.

### Impact

A scalar-only patch against that malformed adapter can crash instead of
failing closed with `DT_REMOTE_ERR_INTERNAL`. Because the failure precedes the
intended validation envelope, the apply API does not preserve its internal
error contract or reliably protect params atomicity for this shape.

### Recommended resolution and test coverage

Guard the traversal with a checked or tri-state probe, or an equivalent
adapter-envelope check. Preserve the fast path for a valid adapter receiving
an unrelated scalar-only patch; the resolution must not force full registry
validation in that case. Add a public apply-level malformed-adapter regression
that demonstrates fail-closed `DT_REMOTE_ERR_INTERNAL` behavior without a
crash and proves the params block remains byte-identical.

No such resolution or regression was implemented in this documentation-only
follow-up.

## VEC3-012: Validate the aggregate native array byte size

### Evidence and root cause

Evidence: `src/control/remote_vector_registry.c:281` and
`src/common/introspection.h:258`.

Registry validation checks the array count, array type, element-descriptor
type, and element size, but does not check the aggregate array field's
`header.size`. The introspection accessor computes element addresses from the
element size and index. A declared three-float array with an eight-byte
aggregate leaf can consequently validate even though later element addressing
extends beyond the declared leaf.

### Impact

Malformed compiled introspection metadata can enter the validity cache and
permit out-of-bounds addressing relative to the declared native leaf. The
existing element-level checks do not establish that the aggregate storage is
large enough.

### Recommended resolution and test coverage

Overflow-safely require
`array_field->header.size == Array.count * sizeof(float)` before caching
validity. Add an isolated synthetic-introspection regression with a
three-float declaration and undersized aggregate leaf; it must reject with
`DT_REMOTE_ERR_INTERNAL` and must fail against the reviewed implementation.

No registry change or synthetic regression was implemented in this
documentation-only follow-up.

## VEC3-013: Reject values outside the native float range before narrowing

### Evidence and root cause

Evidence: `src/control/remote_vector_registry.c:233` and
`src/control/remote_vector.c:145,311`.

Component metadata accepts any finite double bounds, and the pure validator
accepts any finite candidate inside those double-domain bounds. A bound and
candidate such as `1e39` can therefore pass validation and reach an
out-of-range double-to-float conversion.

### Impact

The engine can attempt a native conversion that its validation contract did
not prove representable. Rejection is not guaranteed before a projected write,
and callers cannot rely on finite double-domain acceptance to imply safe native
float storage.

### Recommended resolution and test coverage

Require compiled component bounds to lie within `[-FLT_MAX, FLT_MAX]` and
defensively reject finite candidate values outside the native float range
before conversion. Keep every rejection byte-atomic. Add registry coverage,
pure-validator and public-apply rejection coverage, exact native-range
boundary acceptance coverage, and params/input preservation assertions.

No numeric guard or test was implemented in this documentation-only
follow-up.

## VEC3-014: Preserve LEVELS invariants in the native float domain

### Evidence and root cause

Evidence: `src/control/remote_vector_registry.c:255` and
`src/control/remote_vector.c:168,298-312`.

`minimum_gap == 0` is valid, and double-domain validation accepts strictly
ordered values such as `[0.5, 0.5000000001]`. Both values narrow to the same
float. The current preflight therefore proves ordering and gap only before
narrowing, not for the representations that will be written.

### Impact

An accepted LEVELS patch can lose strict ordering or its declared minimum gap
in native storage. If conversion or destination resolution is interleaved
with writes, a later failure also risks leaving a partially projected vector.

### Recommended resolution and test coverage

After double-domain validation and before any native write, pre-narrow every
component. Verify that the native float representations remain strictly
increasing and still satisfy the descriptor's gap. Resolve and validate every
destination element during the same preflight, then commit the prepared values
only after all checks succeed. Add public apply tests whose close double values
pass the current validator but collapse after narrowing, plus byte-atomic
rejection and native-boundary acceptance coverage.

No LEVELS preflight or regression was implemented in this documentation-only
follow-up.

## VEC3-015: Correct the condition-ownership comment

### Evidence and root cause

Evidence: `src/control/remote_parameters.c:21`.

The comment says curve schemas are the only owners of condition objects, but
vector schemas also own and free active and writable conditions. Runtime
cleanup is already correct; the documentation did not evolve when vector
schema ownership was added.

### Impact

The stale comment can mislead future maintainers about which schema
destructors own condition objects. It does not identify a runtime cleanup
defect.

### Recommended resolution and test coverage

Update the comment to name curve and vector schema ownership. Existing runtime
ownership behavior needs no code change; review the revised wording against
both destructors. No comment change was made in this documentation-only
follow-up.

## Verified strengths

The final whole-range review retained the following strengths from the Task 3
implementation and its historical VEC3-001 through VEC3-010 closure:

- schema conversion deep-copies owned strings and component names and
  preserves registry order;
- value reads are all-or-nothing, widen native floats to doubles, and evaluate
  predicates against the supplied params block;
- apply preflights unknown and duplicate vector IDs, skips other semantic
  classes, evaluates predicates against projected params, writes only exposed
  components, and preserves native tails and unrelated bytes;
- completed validation is positioned after the registry-ordered write loop;
  and
- production sources and the vector unit-test target are included in CMake.

These strengths do not close VEC3-011 through VEC3-015 or change the final
verdict.

## Verification context and disposition

The review was anchored to exact head
`a045d39e8c42e167240cc88df836965d89fd6050`, with a clean worktree and a clean
`git diff --check a29b8a2e76..a045d39e8c42e167240cc88df836965d89fd6050`
range check before this documentation-only closure commit.

Historical verification recorded before this documentation-only commit was:

- direct `test_remote_vector`: 66/66 tests passed;
- focused vector/curve/curve-registry/edit/protocol CTest selection: 5/5
  targets passed; and
- full configured CTest suite: 14/14 targets passed.

The final whole-range review itself was read-only and did not rerun tests.
Those historical passing results do not cover the five newly identified gaps.
Per the user's instruction, this follow-up made no production or test changes
in response and did not create a remediation plan.

**Final reviewer verdict: Ready to merge? No.**
