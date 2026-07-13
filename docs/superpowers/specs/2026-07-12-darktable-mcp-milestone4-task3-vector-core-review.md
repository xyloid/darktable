# darktable MCP Milestone 4 Task 3 Vector Core Review

- **Status:** Closed — remediated and re-reviewed before Milestone 4 Task 4
- **Reviewed:** 2026-07-12
- **Audited plan:** `docs/superpowers/plans/2026-07-12-darktable-mcp-milestone4-vector-class.md`, Task 3
- **Audited range:** `ffd00e2d8a..edb40b6a2e`
- **Implementation commit:** `edb40b6a2e` (`remote_vector: vector-class engine and registry core`)
- **Companion fix plan:** `docs/superpowers/plans/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review-fixes.md`

## Purpose

This document preserves the review findings for Milestone 4 Task 3 separately
from the milestone design and implementation plan. It is the closure contract
for the vector engine and registry core: every finding below must map to a
test-first task in the companion fix plan, and Task 4 must not begin until the
Important findings are closed and the complete C test suite passes.

The production vector adapter table is intentionally empty at this commit.
That limits immediate runtime exposure, but Tasks 5–8 will populate the table;
the registry validator and its tests therefore need to be reliable before
those adapters depend on them.

## Review summary

| ID | Severity | Status | Finding |
|---|---|---|---|
| VEC3-001 | Important | Closed | Registry validation accepts malformed adapter and descriptor metadata that later code dereferences. |
| VEC3-002 | Important | Closed | The pure vector validator accepts NaN and can pass it to native float storage. |
| VEC3-003 | Important (test seam) | Closed | Cached validation can hide or retain a cross-class name collision after lookup overrides change. |
| VEC3-004 | Important | Closed | Subtype-specific descriptor invariants are enforced only in one direction. |
| VEC3-005 | Important (coverage) | Closed | Active and writable predicate tests cannot detect evaluation of the wrong predicate. |
| VEC3-006 | Important (coverage) | Closed | Tests do not prove `validate_completed` observes every completed vector write. |
| VEC3-007 | Important (coverage) | Closed | Native-layout fail-closed tests omit path/type failures and conflate count with capacity. |
| VEC3-008 | Important (coverage) | Closed | The apply path is not tested at the double-to-float range boundary. |
| VEC3-009 | Minor (coverage) | Closed | No-adapter tests are weak and will become invalid when Task 8 registers `borders`. |
| VEC3-010 | Minor (coverage) | Closed | Schema conversion and multi-descriptor engine loops are only partially asserted. |

## VEC3-001: Validate the complete adapter and descriptor envelope

### Evidence

`dt_remote_vector_registry_validate()` reaches loops over
`adapter->vectors` without first validating the adapter's pointer/count pairs:

- duplicate-name loop: `src/control/remote_vector_registry.c:251-261`
- descriptor-validation loop: `src/control/remote_vector_registry.c:315-325`

`vector_descriptor_is_valid()` at
`src/control/remote_vector_registry.c:205-227` validates the introspection
path, native capacity, predicates, and two positive subtype requirements. It
does not validate the semantic name, component metadata, subtype enum range,
component bounds, or prepare-field storage.

A descriptor with a valid `float[3]` native path, `component_count == 3`, and
`components == NULL` therefore validates successfully. Schema conversion then
dereferences `desc->components[j]` at
`src/control/remote_vector_registry.c:413-420`. Other malformed shapes have
similar downstream failures:

- a NULL semantic name can reach `g_str_hash` during value insertion at
  `src/control/remote_vector_registry.c:607-614`;
- `prepare_field_count > 0` with `prepare_fields == NULL` is dereferenced at
  `src/control/remote_vector.c:250-258`;
- non-finite or reversed component bounds make range validation fail open or
  make a component impossible to write.

A minimal external probe against the real `borders` introspection returned
`TRUE` for the valid-path, NULL-components descriptor.

### Impact

Compiled registry mistakes can pass the intended fail-closed gate and become
process crashes or invalid schemas as soon as Tasks 5–8 add production
adapters. The error appears after validation, so the validation cache can also
preserve the unsafe result for the process lifetime.

### Required resolution

Before duplicate, collision, path, schema, read, or apply loops:

- require a nonempty adapter operation equal to `so->op`, an ordered
  parameter-version range, and the loaded introspection version to fall inside
  that range;
- require `vector_count > 0` to have a non-NULL `vectors` array;
- require `prepare_field_count > 0` to have a non-NULL `prepare_fields` array,
  with every listed field name nonempty;
- require every descriptor to have a nonempty semantic name, a supported
  subtype, `component_count > 0`, and a non-NULL `components` array;
- require every component name to be nonempty and every minimum/maximum to be
  finite with `minimum <= maximum`;
- require the resolved native leaf and its array-element descriptor to be
  present and exactly float-backed;
- preserve the existing exact `native_capacity` and predicate checks.

Every invalid shape must return `FALSE` with `DT_REMOTE_ERR_INTERNAL`; no
schema, value, or params byte may be produced or modified.

### Closure tests

- Reject `vector_count > 0` with `vectors == NULL` without crashing.
- Reject an operation mismatch, a reversed version range, and an introspection
  version outside the adapter range.
- Reject `prepare_field_count > 0` with `prepare_fields == NULL`.
- Reject NULL and empty descriptor names.
- Reject `component_count == 0` and nonzero count with `components == NULL`.
- Reject NULL/empty component names, non-finite bounds, and minimum greater
  than maximum.
- Reject a float array whose element descriptor is missing.
- Confirm the real `borders` fixture descriptor remains valid.

## VEC3-002: Reject every non-finite vector component in the pure validator

### Evidence

`dt_remote_vector_validate()` at `src/control/remote_vector.c:144-157` checks
only:

```c
value < component->minimum || value > component->maximum
```

Both comparisons are false for NaN. For a LEVELS descriptor, the later
`delta <= 0.0` and `delta < minimum_gap` checks are also false when the delta
is NaN. A direct probe returned `TRUE` with no error for a one-component NaN
vector.

The protocol parser rejects non-finite JSON before the engine runs, but
`dt_remote_vector_validate()` is a public internal pure validator and
`dt_remote_vector_apply_patch()` trusts it before narrowing values to native
floats. The curve twin explicitly checks `isfinite()` in
`src/control/remote_curve.c:205-220`.

### Impact

Any internal caller that constructs a semantic patch without the JSON parser
can write NaN to a native params block. The validator also violates its stated
inclusive-domain contract: NaN belongs to no finite component interval.

### Required resolution

Check `isfinite(value)` before component range and LEVELS ordering checks.
Reject every non-finite component with `DT_REMOTE_ERR_INVALID_VALUE`, the
semantic parameter name, component index, and stable constraint
`"non_finite"`. Do not modify the input `GArray`.

### Closure tests

- Reject NaN, positive infinity, and negative infinity for a PLAIN vector.
- Reject NaN in each position of a LEVELS vector.
- Assert the parameter, component index, and `non_finite` constraint details.
- Confirm exact finite minimum and maximum boundaries remain accepted.
- Confirm rejection never modifies the input array or native params.

## VEC3-003: Keep cross-class uniqueness correct when overrides change

### Evidence

The cache hit at `src/control/remote_vector_registry.c:286-296` returns before
the curve/vector name-collision check at
`src/control/remote_vector_registry.c:308-313`.

The cache key contains only vector adapter identity and introspection parameter
version, while collision status also depends on the current result of
`dt_remote_curve_registry_lookup()`. That lookup is mutable through the
test-only override. The following sequence was reproduced:

1. Validate vector adapter A with no curve override: `TRUE` is cached.
2. Install a curve override exposing A's semantic ID.
3. Validate A again: cached `TRUE` is returned, hiding the collision.

The reverse order leaves an adapter cached invalid after the colliding curve
override is removed.

### Impact

Production curve and vector tables are immutable, so this is not a production
registry race. It does make the explicit test seam stateful across tests and
can allow the milestone's cross-class uniqueness gate to pass or fail based on
test order.

### Required resolution

Cache only validation facts that depend on immutable vector adapter data and
module introspection. Re-run cross-class semantic-name comparison on every
`dt_remote_vector_registry_validate()` call after the intrinsic cached result
passes. A collision must never be inserted into the intrinsic layout cache.

### Closure tests

- Validate A successfully, install a colliding curve override, and then reject
  A without changing its address or parameter version.
- Remove the override and validate A successfully again.
- Start with the collision, reject A, remove the override, and then validate A
  successfully.
- Confirm repeated intrinsic layout validation still uses its cached result.

## VEC3-004: Enforce exhaustive subtype invariants

### Evidence

`vector_descriptor_is_valid()` at
`src/control/remote_vector_registry.c:222-223` rejects COLOR without a
`color_space` and LEVELS without `strictly_increasing`. It still accepts:

- enum values outside `DT_REMOTE_VECTOR_PLAIN` through
  `DT_REMOTE_VECTOR_LEVELS`;
- `color_space` on PLAIN and LEVELS despite the documented
  non-NULL-if-and-only-if-COLOR contract;
- an empty COLOR `color_space`;
- negative or non-finite LEVELS `minimum_gap`;
- LEVELS-only ordering metadata on non-LEVELS subtypes.

### Impact

Task 4 serializers switch on the subtype and conditionally emit color and
ordering members. An invalid subtype can therefore become an assertion or an
invalid wire schema, while contradictory metadata can be silently discarded.
A NaN minimum gap also disables the advertised spacing constraint.

### Required resolution

Use an exhaustive subtype switch during registry validation:

- PLAIN: forbid `color_space`, `strictly_increasing`, and nonzero
  `minimum_gap`;
- COLOR: require a nonempty `color_space` and forbid LEVELS-only metadata;
- LEVELS: forbid `color_space`, require `strictly_increasing`, and require a
  finite, nonnegative `minimum_gap`;
- default: reject the descriptor.

### Closure tests

- Reject an unknown subtype enum value.
- Reject empty COLOR space and accept `"display_rgb"`.
- Reject color space on PLAIN and LEVELS.
- Reject negative, NaN, and infinite LEVELS gaps; accept `0.0` and
  `FLT_EPSILON`.
- Reject LEVELS-only metadata on PLAIN and COLOR.

## VEC3-005: Test active and writable predicates independently

### Evidence

The read test assigns the same predicate to `active_when` and
`writable_when` at `src/tests/unittests/control/test_remote_vector.c:926-939`.
The projected-write test repeats that pattern at
`src/tests/unittests/control/test_remote_vector.c:1298-1313`.

An implementation that ignores `writable_when` and reuses `active_when` would
pass both tests.

### Required resolution and closure tests

- Read a descriptor with distinct/opposing active and writable predicates and
  assert all four Boolean combinations across live params.
- Test the projected writable-state transition with `active_when == NULL` and
  only `writable_when` gated by the scalar prepare field.
- Retain a separate active-only rejection test so apply must evaluate both
  predicates rather than either one twice.

## VEC3-006: Prove completed validation runs after every vector write

### Evidence

The test callback at `src/tests/unittests/control/test_remote_vector.c:558-575`
ignores `new_params`. Tests count calls or force unconditional failure, but
none fail if the callback runs before vector writes.

The rollback assertion at
`src/tests/unittests/control/test_remote_vector.c:1386-1425` confirms only
that the separate live params allocation remains unchanged while the helper
mutates a projected copy. That matches the intended caller-owned rollback
boundary, but it does not prove callback timing or multi-entry completion.

### Required resolution and closure tests

- Use two descriptors backed by `color` and `frame_color` in one patch.
- Make `validate_completed` resolve both leaves from `new_params` and reject
  unless it observes both requested values.
- Assert the callback is invoked once, after registry-ordered writes.
- Add a rejecting completed-state test that verifies the projected block may
  contain candidate writes but the live block remains byte-identical because
  the caller discards the projection.

## VEC3-007: Isolate every native-layout fail-closed branch

### Evidence

There is no registry test for a non-resolving path, a scalar leaf, or a
non-float array leaf. In addition,
`test_registry_validate_rejects_native_capacity_smaller_than_component_count`
at `src/tests/unittests/control/test_remote_vector.c:640-661` sets both
`component_count` and `native_capacity` to 4 against a real length-3 array.
An implementation that checks only exact capacity would still reject that
case, so the test does not isolate the required
`resolved_length >= component_count` check.

### Required resolution and closure tests

- Reject a missing native field path.
- Reject a path ending at a scalar float rather than an array.
- Reject a path ending at a non-float array.
- Isolate count overflow with `component_count == 4` and
  `native_capacity == resolved_length == 3`.
- Isolate exact-capacity mismatch with a component count that fits the real
  array while `native_capacity` differs.
- Assert every failure is `DT_REMOTE_ERR_INTERNAL` and leaves outputs NULL.

## VEC3-008: Prove double-domain validation in the apply path

### Evidence

The pure validator test uses the required `2.00000001` boundary, but
`test_apply_patch_domain_violation_rejects_and_leaves_live_params_untouched`
at `src/tests/unittests/control/test_remote_vector.c:1181-1207` uses the coarse
out-of-range value `1.5`.

An apply implementation that narrows to float before validation would pass the
current apply tests even though `1.00000001` rounds to an in-range `1.0f`.

### Required resolution and closure tests

- Apply `1.00000001` against a component maximum of `1.0` and expect
  `DT_REMOTE_ERR_INVALID_VALUE` before any native write.
- Snapshot the complete projected and live params blocks and assert both remain
  unchanged for this pre-write rejection.
- Retain the successful narrowing test and exact-maximum acceptance test.

## VEC3-009: Make no-adapter tests independent of production registry growth

### Evidence

The no-adapter schema test initializes `fields` to NULL at
`src/tests/unittests/control/test_remote_vector.c:809-822`; returning TRUE
without assigning `*out_fields` would pass. The schema, read, and apply
no-adapter tests also depend on the production table having no `borders`
adapter. Milestone 4 Task 8 intentionally registers `borders`, so those tests
will cease to exercise the behavior they name.

### Required resolution and closure tests

- Install a lookup override that always returns NULL for every no-adapter test.
- Initialize schema and value output pointers to non-NULL sentinels.
- Assert schema listing overwrites its sentinel with NULL.
- Assert value reading overwrites its sentinel with a non-NULL empty owned
  table.
- Assert a vector patch returns `DT_REMOTE_ERR_UNKNOWN_FIELD`, while a patch
  with no vector content remains a successful no-op.

## VEC3-010: Complete schema and multi-descriptor loop coverage

### Evidence

The two-descriptor schema test at
`src/tests/unittests/control/test_remote_vector.c:824-867` proves registry
order and a subset of subtype/component fields. It does not verify
descriptions, component ranges, LEVELS ordering metadata, or owned predicate
copies. Successful read and apply tests use only one distinct descriptor, so
the per-descriptor loops are not independently proven.

### Required resolution and closure tests

- Assert every schema member copied from a descriptor, including description,
  per-component minimum/maximum, subtype metadata, writability, and both
  owned conditions.
- Prove ownership by freeing the returned schema through its public destructor
  with all optional fields populated.
- Read two descriptors backed by distinct native arrays and assert both values
  and statuses.
- Apply two vector entries supplied in reverse request order and assert native
  writes and completed validation still follow registry order.

## Verified strengths

- Schema conversion deep-copies owned strings and component names and preserves
  registry order.
- Value reads are all-or-nothing, widen native floats to doubles, and evaluate
  predicates against the supplied params block.
- Apply preflights unknown and duplicate vector IDs, skips other semantic
  classes, evaluates predicates against projected params, writes only exposed
  components, and preserves native tails and unrelated bytes.
- `validate_completed` is implemented after the registry-ordered write loop;
  the remaining issue is making that placement regression-sensitive.
- CMake includes both production sources and the vector unit-test target.

## Verification evidence

The review ran the following commands against `edb40b6a2e`:

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
git diff --check ffd00e2d8a..edb40b6a2e
git status --short
```

Results:

- build succeeded;
- all 14 configured CTest targets passed, including `test_remote_vector`;
- `git diff --check` produced no errors;
- the source worktree was clean after review.

Passing tests do not close the findings above: the reproduced failures use
inputs and override sequences absent from the current suite.

### Remediation and re-review

The complete remediation range is
`edb40b6a2e..e53abacc13ae50fd35f6f845b74e381b4c18a9aa`.

Closure verification results:

- the focused five-target build exited 0;
- the direct `test_remote_vector` binary passed 66/66 tests;
- the focused vector/curve/curve-registry/edit/protocol CTest selection passed
  5/5 targets;
- the full configured CTest suite passed 14/14 targets;
- `git diff --check edb40b6a2e..e53abacc13ae50fd35f6f845b74e381b4c18a9aa`
  completed cleanly;
- the fresh broad review at exact head
  `e53abacc13ae50fd35f6f845b74e381b4c18a9aa` closed VEC3-001 through
  VEC3-010, reported no Critical, Important, or Minor findings, and concluded
  **Ready to merge: Yes**.

## Closure gate

Milestone 4 Task 3 is ready to continue only when:

- every `VEC3-*` item maps to and passes its companion-plan closure tests;
- the vector registry rejects malformed compiled metadata without a crash or
  partial output;
- the pure validator rejects every non-finite component;
- cross-class uniqueness is independent of lookup-override test order;
- all focused vector, curve-regression, remote-edit, and protocol tests pass;
- the full configured CTest suite passes; and
- a fresh code review reports no remaining Critical or Important findings in
  the Task 3 range plus its remediation commits.
