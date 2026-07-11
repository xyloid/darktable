# darktable MCP Tasks 5–7 Review Fixes — Tracking Plan

**Status:** Not started

**Scope:** Fix every finding from the review of Milestone 2 Tasks 5 and 6, plus the unfinished Task 7 implementation.

**Related plan:** `docs/superpowers/plans/2026-07-11-darktable-mcp-milestone2-denylist-rgbcurve.md`

## Findings covered

- Registry validation errors are swallowed by schema and value reads.
- Predicate drift and unknown live enum values can fail open.
- The registry validation cache is keyed only by adapter, not parameter version.
- Out-of-range interpolation values can trigger undefined shift behavior.
- `dt_remote_get_module_schema()` suppresses semantic registry errors.
- `curve_params` and writable curve schemas are advertised before curve mutation is accepted.
- Task 7 lacks the required MCP sidecar passthrough tests.

## Global constraints

- Registry drift fails closed with `DT_REMOTE_ERR_INTERNAL`; no partial semantic schema or value result is returned.
- Primitive-only mutation remains available when semantic support for an operation is invalid.
- No validation path clamps, sorts, deduplicates, inserts, drops, or otherwise modifies points.
- `curve_params` must not be advertised until a valid `semantic_values` request can reach the atomic mutation engine.
- Operations without semantic adapters retain their existing wire responses.
- Protocol version remains 1.

---

## Task 1: Harden interpolation validation

**Files:**

- Modify: `src/control/remote_curve.c`
- Test: `src/tests/unittests/control/test_remote_curve.c`

- [ ] Add an explicit range check for supplied `dt_remote_curve_interpolation_t` values before computing a mask bit.
- [ ] Return `DT_REMOTE_ERR_INVALID_VALUE` with `constraint: "interpolation_not_allowed"` for negative values.
- [ ] Return the same error for values greater than or equal to the bit width of `interpolation_mask`.
- [ ] Perform `1u << interpolation` only after the range check succeeds.
- [ ] Add a test for `(dt_remote_curve_interpolation_t)-1`.
- [ ] Add a test for an oversized interpolation value.
- [ ] Confirm the existing allowed, disallowed, and omitted-interpolation tests still pass.
- [ ] Run `ctest --test-dir build -R test_remote_curve --output-on-failure`.

**Acceptance:** No caller-controlled interpolation value can cause an invalid shift, and every invalid value is rejected without modifying the point array.

---

## Task 2: Make registry validation comprehensive and version-aware

**Files:**

- Modify: `src/control/remote_curve_registry.c`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c`

- [ ] Replace the adapter-pointer-only cache key with `(adapter identity, params_version)`.
- [ ] Cache one aggregate valid/invalid result for each adapter/version pair.
- [ ] Remove per-descriptor partial-success behavior.
- [ ] Validate every descriptor's nodes, count, and interpolation paths.
- [ ] Validate that the node element is a struct and that `x`/`y` are supported floating leaves.
- [ ] Validate that native node capacity is at least `maximum_points`.
- [ ] Validate the optional internal-version path when present.
- [ ] Validate `active_when`, `writable_when`, and `periodic_when` predicate fields.
- [ ] Reject missing predicate fields.
- [ ] Reject predicate fields that are not enums.
- [ ] Reject invalid predicate operators.
- [ ] Resolve each configured predicate enum name through introspection and reject unknown names.
- [ ] Add a test proving a bad version-1 introspection does not poison a good version-2 result for the same adapter.
- [ ] Add a test proving repeated validation of one adapter/version uses its cached result.
- [ ] Add tests for missing fields, wrong field types, unknown enum names, and insufficient native capacity.
- [ ] Run `ctest --test-dir build -R test_remote_curve_registry --output-on-failure`.

**Acceptance:** Validation is isolated per parameter version, checks all compiled registry data, and disables the entire adapter when any descriptor is invalid.

---

## Task 3: Make semantic schema and value reads fail closed

**Files:**

- Modify: `src/control/remote_curve_registry.c`
- Modify: `src/control/remote_curve.h`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c`

- [ ] Make `dt_remote_curve_list_schema()` propagate registry validation failures.
- [ ] Free temporary results and leave `*out` unset on failure.
- [ ] Never return a partial semantic schema list.
- [ ] Make `dt_remote_curve_read_values()` propagate registry validation failures.
- [ ] Abort with `DT_REMOTE_ERR_INTERNAL` on predicate-resolution or path-resolution failure.
- [ ] Never silently mark a predicate inactive because resolution failed.
- [ ] Never silently omit a failed descriptor.
- [ ] Treat an unknown live enum value as `DT_REMOTE_ERR_INTERNAL`; an `NE` predicate must not become true when enum-name resolution returns `NULL`.
- [ ] Treat a missing `writable_when` predicate as unconditional writability (`DT_REMOTE_WRITABLE_NOW` / `writable_now == TRUE`).
- [ ] Resolve `periodic_when` when producing live semantic values.
- [ ] Add a real-rgbcurve test with an unknown raw `curve_autoscale` value and expect `DT_REMOTE_ERR_INTERNAL`.
- [ ] Add an unconditional synthetic descriptor test for schema and live writability.
- [ ] Confirm automatic/manual RGB polarity and inactive-value behavior remain unchanged.
- [ ] Run `ctest --test-dir build -R test_remote_curve_registry --output-on-failure`.

**Acceptance:** A semantic read either returns the complete validated adapter result or an internal error; it never returns a partial or fail-open result.

---

## Task 4: Propagate semantic failures through remote_edit

**Files:**

- Modify: `src/control/remote_edit.c`
- Modify: `src/control/remote_edit.h`
- Test: `src/tests/unittests/control/test_remote_edit.c`
- Test: `src/tests/unittests/control/test_remote_protocol.c`
- Fixture: add an internal-error schema response fixture if useful for the dispatcher pattern

- [ ] Stop freeing and suppressing `curve_error` in `dt_remote_get_module_schema()`.
- [ ] On semantic schema failure, free the partially constructed module schema.
- [ ] Leave `*out` unset, propagate the original error, and return `FALSE`.
- [ ] Keep primitive-only mutation independent of semantic registry validity.
- [ ] Retain fail-closed semantic handling in `dt_remote_get_module_params()`.
- [ ] Add a final-process rgbcurve schema test that temporarily introduces introspection drift, expects `DT_REMOTE_ERR_INTERNAL`, and restores the introspection immediately.
- [ ] Add a dispatcher test proving an internal schema error produces an error envelope, not a successful primitive-only schema.
- [ ] Assert all failure output pointers remain `NULL`.
- [ ] Run `ctest --test-dir build -R 'test_remote_(edit|protocol)' --output-on-failure`.

**Acceptance:** Registry drift is visible as an actionable internal error at the wire boundary, while unrelated primitive mutation paths remain functional.

---

## Task 5: Reorder curve capability activation

**Files:**

- Modify later: `src/control/remote_protocol.c`
- Modify later: `src/tests/unittests/control/fixtures/hello_response.json`
- Depends on: Milestone Tasks 8 and 9 mutation engine and `semantic_values` parsing

- [ ] Do not merge or ship the current Task 7 state independently.
- [ ] Move production hello advertisement of `semantic_params` and `curve_params` to the activation step after Tasks 8 and 9 are complete.
- [ ] Do not advertise curve schemas as writable until semantic mutation is accepted and applied atomically.
- [ ] Complete Task 8's semantic mutation engine.
- [ ] Complete Task 9's `semantic_values` parser and dispatcher integration.
- [ ] Verify a valid semantic curve request reaches the mutation engine.
- [ ] Activate `semantic_params`, `curve_params`, semantic schema/value members, and writable curve schemas together.
- [ ] Restore the `represented_by` threading in `dt_remote_get_module_schema()` (dropped during the Task 4 rewrite of `remote_edit.c`) in the same activation change, and cover it with a final-process test so live responses match the fixtures.
- [ ] Update the hello fixture only in that activation change.
- [ ] Add a regression test: whenever hello contains `curve_params`, a valid `semantic_values` request must not fail unknown-key validation.

**Acceptance:** There is no mergeable or deployable state in which the server advertises curve editing but rejects the advertised request member.

---

## Task 6: Complete Task 7 sidecar passthrough coverage

**Files:**

- Modify: `tools/mcp/tests/test_protocol.py`
- Modify: `tools/mcp/tests/test_tools.py`
- Consume:
  - `src/tests/unittests/control/fixtures/get_module_schema_rgbcurve_response.json`
  - `src/tests/unittests/control/fixtures/get_module_params_rgbcurve_response.json`

- [ ] Add both rgbcurve fixtures to the shared protocol passthrough coverage.
- [ ] Add a tool test proving `get_module_schema` returns `semantic_fields` verbatim.
- [ ] Assert native fields retain `represented_by` verbatim.
- [ ] Add a tool test proving `get_module_params` returns all four `semantic_values` verbatim.
- [ ] Assert inactive curves remain present with `active: false`.
- [ ] Assert point objects and uppercase interpolation names remain unchanged.
- [ ] Retain the existing exposure tests and confirm primitive-only responses omit semantic members.
- [ ] Run `cd tools/mcp && .venv/bin/pytest -q`.

**Acceptance:** Both the protocol client and MCP tools demonstrably pass the new semantic response members through without reshaping or dropping them.

---

## Recommended commit order

- [x] `remote_curve: harden curve validation and versioned registry checks`
- [x] `remote_curve: fail closed on descriptor and predicate drift`
- [x] `remote_edit: propagate semantic registry failures`
- [x] Milestone Task 8 mutation commit (`remote_curve: rgbcurve adapter callbacks and atomic apply path`).
- [ ] Milestone Task 9: `remote_protocol: semantic_values mutation parsing and read-back`
- [ ] `remote_protocol: activate semantic curve capabilities and read responses`
- [ ] `darktable-mcp: cover semantic schema and value passthrough`

## Execution order (agreed 2026-07-11)

1. **Milestone Task 9** — `semantic_values` request parsing, dispatch through the
   Task 8 transaction, read-back serialization, six fixtures, dispatcher tests.
   This commit may include the shared curve-value serializer helpers but must not
   advertise capabilities or emit semantic schema/value members on read responses.
2. **Activation commit** — hello `semantic_params`/`curve_params` capabilities,
   hello fixture update, `semantic_fields`/`semantic_values` read-response members,
   `represented_by` on native array fields (schema struct member, live threading in
   `remote_edit.c`, wire serialization), rgbcurve read fixtures and dispatcher
   tests, and the hello-implies-parser regression test. One commit, per Task 5.
3. **Review Task 6** — sidecar pytest passthrough coverage of both rgbcurve read
   fixtures (milestone Task 7 Step 4).
4. **Milestone Tasks 10–11** — sidecar `curves` argument with capability gating,
   then live integration, CI, and docs.

Every commit should end with:

```text
Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
```

## Verification matrix

### Focused build

```bash
cmake --build build --target \
  test_remote_curve \
  test_remote_curve_registry \
  test_remote_edit \
  test_remote_protocol
```

### Focused C tests

```bash
ctest --test-dir build \
  -R 'test_remote_(curve|curve_registry|edit|protocol)' \
  --output-on-failure
```

### Full remote C suite

```bash
ctest --test-dir build -R 'test_remote_' --output-on-failure
```

### MCP unit suite

```bash
cd tools/mcp
.venv/bin/pytest -q
```

### Optional sanitizer check

- [ ] Run the focused curve validator suite under UBSan when a sanitizer build is available.

## Final acceptance checklist

- [ ] Registry drift always produces `internal`, never partial semantic output.
- [ ] Missing or invalid predicates cannot become active or writable.
- [ ] Unknown live enum values fail closed.
- [ ] Registry cache results cannot cross parameter versions.
- [ ] Invalid interpolation cannot trigger undefined behavior.
- [ ] `curve_params` is never advertised before semantic mutation works.
- [ ] Existing non-curve fixtures remain unchanged.
- [ ] rgbcurve schema/value fixtures pass through the C dispatcher and MCP sidecar verbatim.
- [ ] All focused and full remote C suites pass.
- [ ] The MCP unit suite passes.
- [ ] The unrelated `test_filmicrgb` linker failure is resolved or separately documented before the milestone is declared fully green.
