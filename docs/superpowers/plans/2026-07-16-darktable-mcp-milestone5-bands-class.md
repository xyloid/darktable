# darktable MCP Milestone 5 — Sampled-Response (Bands) Class — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the sampled-response semantic class (wire name `bands`)
and four module adapters — atrous, denoiseprofile, rawdenoise, lowlight —
plus the composed-dispatch restructure and the sidecar error-mapping
cleanup, per the approved design
`docs/superpowers/specs/2026-07-16-darktable-mcp-milestone5-bands-class-design.md`.
That spec is normative; where this plan says "per spec §X" the implementer
must read that section.

**Architecture:** Milestones 2–4 built two parallel class pipelines: parser
(`remote_protocol.c`) → tagged patch/schema/value envelopes
(`remote_parameters.h`) → engine (`remote_curve.c` / `remote_vector.c`) →
registry (static descriptors + callbacks) → explicit two-call sequences at
four seams in `remote_edit.c` → JSON serializers → Python sidecar.
Milestone 5 first replaces the two-call seams with a class-ops dispatch
table (curve/vector behavior byte-identical — the m4 design deferred this
exact vtable "until milestone 5 adds a third class"), then adds a band
engine + registry reusing the generic pieces (`dt_remote_path_resolve`,
predicates), and ships four static adapters. Doubles end-to-end until
narrowing; whole-set replacement; reject-don't-clamp.

**Tech Stack:** C (GLib, JSON-GLib, darktable introspection), cmocka unit
tests under `src/tests/unittests/control/`, Python 3 + FastMCP sidecar in
`tools/mcp/`, pytest integration harness under Xvfb.

## Global Constraints

- Work in the git worktree `<REPO>/.claude/worktrees/mcp-remote-edit`, branch `worktree-mcp-remote-edit`. All paths below are relative to that root.
- TDD: every behavior change lands with its failing test first. Pure refactors (Task 1) are covered by the existing suites staying green.
- Commit messages end with the two trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`.
- Verification at every commit: `cmake --build build -j$(nproc)` then
  `ctest --test-dir build` (all suites), `cd tools/mcp && .venv/bin/pytest -q`;
  for Task 9 also `DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`.
- **Curve AND vector behavior byte-for-byte unchanged.** The existing
  `test_remote_curve` (69 tests), `test_remote_vector` (109 tests),
  protocol/edit suites, and the sidecar suites are the regression net. If
  any of them fails, the change is wrong — stop.
- Params versions (verified from `DT_MODULE_INTROSPECTION`): `atrous` = 2,
  `rawdenoise` = 2, `denoiseprofile` = 12, `lowlight` = 1. Every adapter
  pins `minimum_params_version == maximum_params_version` to exactly these.
- Stable wire vocabulary (spec §Wire and MCP contract): class `"bands"`;
  x policies `"fixed"`, `"interior"`; capability `"band_params"`; gap
  comparison `"at_least"`; schema members `count`, `x_policy`, `min_gap`,
  `x_shared_with`; error detail `band_index`.
- Band samples travel as **doubles** (`GArray` of `double`) from parser
  through validation; narrow to `float` only when writing the blob.
- Wire patch entries require `"class": "bands"` — the parser's "class is
  required in mutation values" rule applies, exactly as it does for curves
  and vectors.
- y range for all four modules: **[0.0, 1.0] inclusive**, descriptor-carried
  (the GUI clamp, e.g. `atrous.c:1053`).
- atrous interior-x rules: endpoints pinned (wire x[0]/x[N−1] must equal the
  stored endpoints after `(float)` narrowing); strictly ascending with
  **absolute minimum gap `0.001`, at-least comparison** (the GUI's neighbor
  clamp, `atrous.c:1406-1408`); twin groups luma↔luma_threshold and
  chroma↔chroma_threshold share x — an x write mirrors to the twin's native
  array; two entries in one request writing different x to one twin group
  are rejected.
- lowlight interior-x rules (amended, see the resolved-decisions bullet
  below): same pinning/ascending/0.001-at-least gap as atrous (the
  identical GUI neighbor clamp, `lowlight.c:684-686`), no twins.
- Band counts: atrous 6, denoiseprofile 7, rawdenoise 5, lowlight 6.

## Decisions resolved from the spec's "Open items"

- **Wire spellings are snake_case**, matching milestone 4: `x_policy`,
  `min_gap`, `x_shared_with`, `band_index`, `y_range`.
- **Enum rename:** milestone 4 reserved `DT_REMOTE_PARAMETER_SAMPLED_RESPONSE`
  with zero uses. Task 2 renames it to `DT_REMOTE_PARAMETER_BANDS` so the
  enum matches the wire class string. No compatibility concern — grep
  proves it unused before renaming.
- **The shared final pass does not subsume per-engine version pins.** Each
  engine keeps its registry version check (proven, fails closed); the
  shared pass is the existing post-apply verification in `remote_edit.c`.
  Parity gates decide, per spec.
- **denoiseprofile v12 layout:** the adapter touches only the `x`/`y`
  2-D float arrays via introspection paths; no `struct.pack` byte poking,
  so padding is introspection's problem, not ours. Task 6's registry
  validation (resolved array length == native capacity) is the guard.
- **Error precedence documentation lives in the protocol reference**
  (Task 10), one paragraph: parse errors first, then engine dispatch order
  curve → vector → bands.
- **lowlight x policy amended to interior (post-Task-6 review).** The spec
  originally classified lowlight `fixed`; implementation review found the
  GUI's x-drag strip below the curve (`lowlight_motion_notify`,
  `lowlight.c:684-686`) moves interior `transition_x` nodes with pinned
  endpoints and the same 0.001 at-least neighbor clamp as atrous, and the
  shipped "night blooming" preset stores a non-default x
  (`lowlight.c:424`). Spec §Adapters and Tasks 5/9 amended: the lowlight
  descriptor is `DT_REMOTE_BAND_X_INTERIOR`, `minimum_gap` 0.001, no
  twins.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/control/remote_edit.c` | modify | Task 1: class-ops dispatch table replacing the four two-call seams; Task 4: bands arm; Task 7: atrous denylist row |
| `src/control/remote_curve.h/.c`, `remote_vector.h/.c` | modify | Task 1: `apply_entries` variants taking a pre-partitioned entry slice |
| `src/control/remote_parameters.h/.c` | modify | enum rename; band patch/schema/value types; wrapper `bands` union arms |
| `src/control/remote_protocol.c` | modify | parser branch for class `"bands"`; `_band_schema_to_json` / `_band_value_to_json`; `band_params` in hello |
| `src/control/remote_band.h/.c` | create | band engine: descriptor/adapter types, `list_schema`, `read_values`, `apply_entries`, pure validator |
| `src/control/remote_band_registry.c` | create | static adapters for the four modules; `registry_lookup`, `registry_validate` (incl. cross-class ID uniqueness) |
| `src/tests/unittests/control/test_remote_band.c` | create | engine + per-adapter unit tests |
| `src/tests/unittests/control/test_remote_protocol.c` | modify | bands parser hygiene tests |
| `src/tests/unittests/control/CMakeLists.txt` | modify | register `test_remote_band` |
| `tools/mcp/src/darktable_mcp/server.py` | modify | `bands` argument, `_wire_band_values`, `band_params` gate; `_wire_semantic_values` ValueError → ToolError |
| `tools/mcp/tests/test_tools.py`, `tests/test_protocol.py` | modify | sidecar unit tests |
| `tools/mcp/tests/integration/test_bands_milestone5.py` | create | live gates |
| docs (see Task 10) | modify | tiers, milestone row, appendix, protocol reference, README/remote-control |

The CMake source list that builds `remote_vector_registry.c` (find it with
`grep -rn "remote_vector_registry" src/CMakeLists.txt src/control/CMakeLists.txt`)
gains the two new C files in Task 3.

---

### Task 1: Composed dispatch — class-ops table (curve/vector refactor)

Spec §Composed-validation seam. Pure refactor, wire byte-identical; the
existing suites are the proof. The m4 plan explicitly deferred this vtable
to "when milestone 5 adds a third" class — that is now.

**Files:**
- Modify: `src/control/remote_edit.c` — the four seams (find them:
  `grep -n "dt_remote_curve_list_schema\|dt_remote_curve_read_values\|dt_remote_curve_apply_patch\|dt_remote_vector_list_schema\|dt_remote_vector_read_values\|dt_remote_vector_apply_patch" src/control/remote_edit.c`)
- Modify: `src/control/remote_curve.h/.c`, `src/control/remote_vector.h/.c`

**Interfaces (Produces):**

```c
/* remote_edit.c (file-local) */
typedef struct dt_remote_class_ops_t
{
  dt_remote_parameter_class_t class_id;
  gboolean (*list_schema)(const struct dt_iop_module_so_t *so,
                          GPtrArray **out_fields, dt_remote_error_t **error);
  gboolean (*read_values)(const struct dt_iop_module_t *module,
                          const void *params, GHashTable **out,
                          dt_remote_error_t **error);
  gboolean (*apply_patch)(const struct dt_iop_module_t *module,
                          const void *old_params, void *new_params,
                          const dt_remote_patch_t *patch,
                          dt_remote_error_t **error);
  /* apply_patch takes the FULL patch — the engines' exported wrappers keep
     their internal own-class partitioning and the prepare_needed gate
     (scalar-only patches naming a prepare_field must still reach the
     engine). The dispatcher calls every row unconditionally in table
     order, exactly as the pre-refactor code called both engines. */
  dt_remote_semantic_schema_t *(*wrap_schema)(gpointer class_schema);
  dt_remote_semantic_value_t *(*wrap_value)(gpointer class_value);
} dt_remote_class_ops_t;

static const dt_remote_class_ops_t s_class_ops[] = { /* curve, vector */ };

/* remote_curve.h — new; the existing dt_remote_curve_apply_patch body
   becomes: partition its own entries out of the patch, call this. */
gboolean dt_remote_curve_apply_entries(const struct dt_iop_module_t *module,
                                       const void *old_params, void *new_params,
                                       GPtrArray *entries,
                                       dt_remote_error_t **error);
/* remote_vector.h — same shape */
gboolean dt_remote_vector_apply_entries(const struct dt_iop_module_t *module,
                                        const void *old_params, void *new_params,
                                        GPtrArray *entries,
                                        dt_remote_error_t **error);
```

- [ ] **Step 1: Extract `apply_entries` in both engines.** In
  `remote_curve.c`, split `dt_remote_curve_apply_patch` into a thin wrapper
  (builds a `GPtrArray` view of the patch's curve-class entries in request
  order, calls `dt_remote_curve_apply_entries`, frees the view — entries
  are borrowed, `g_ptr_array_new()` without free func) and the entry loop
  itself, which now iterates the given slice and **asserts** (does not
  skip) that every entry's `class_id` is curve. Preserve the internal
  ordering: prepare-field composition, per-entry validation, and
  `validate_completed` all stay inside `apply_entries`, exactly where they
  are today. Same split in `remote_vector.c`. Keep the old `apply_patch`
  symbols exported — the unit tests call them directly.
- [ ] **Step 2: Build the class-ops table and convert the four seams.** In
  `remote_edit.c`, define `s_class_ops[]` with the curve and vector rows in
  that order. Convert each seam from two explicit calls to a loop over the
  table:
  - schema listing: for each ops row, `list_schema`, wrap each result with
    the row's `wrap_schema`, append (existing insertion order preserved:
    curve rows first, then vector — identical to today).
  - readback: same loop shape with `read_values`/`wrap_value`.
  - apply: call every row's `apply_patch` (the existing exported wrapper)
    unconditionally in table order against the same `temp_params`. Each
    wrapper partitions its own class's entries internally and keeps the
    `prepare_needed` gate, so a scalar-only patch naming a prepare_field
    (e.g. colorzones `channel`) still reaches its engine. Do NOT gate the
    call on "has entries of this class" — that would skip the gate and
    change production behavior. The existing post-apply verification stays
    after the loop unchanged — that is the shared final pass.
  - `represented_by` annotation: convert its curve/vector pair the same way
    (loop over table rows' registries; keep a per-row lookup function
    pointer if the current code needs one — follow what's there).
- [ ] **Step 3: Build and verify byte-identical behavior.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build && cd tools/mcp && .venv/bin/pytest -q`
  Expected: all pass, zero test edits.
- [ ] **Step 4: Commit** — `remote_edit: class-ops dispatch table over curve/vector engines (parity refactor)`

### Task 2: Bands class core — enum rename, patch arm, parser branch

Spec §Wire and MCP contract (mutation entry). Mirrors m4 Task 2.

**Files:**
- Modify: `src/control/remote_parameters.h/.c`
- Modify: `src/control/remote_protocol.c` (the class dispatch — find it:
  `grep -n '"vector"' src/control/remote_protocol.c`, the parser branch)
- Test: `src/tests/unittests/control/test_remote_protocol.c`

**Interfaces (Produces):**

```c
/* remote_parameters.h */
DT_REMOTE_PARAMETER_BANDS   /* renamed from DT_REMOTE_PARAMETER_SAMPLED_RESPONSE */

typedef struct dt_remote_band_patch_t
{
  char *name;      // owned semantic ID
  GArray *y;       // double elements, owned, required
  GArray *x;       // double elements, owned, NULL when absent
} dt_remote_band_patch_t;
/* added to dt_remote_semantic_patch_t's union as `bands` */

/* remote_protocol.c */
#define DT_REMOTE_BAND_WIRE_SAMPLE_CAP 8
```

- [ ] **Step 1: Rename the reserved enum value.** Prove it is unused
  (`grep -rn "SAMPLED_RESPONSE" src/ tools/`), then rename to
  `DT_REMOTE_PARAMETER_BANDS`.
- [ ] **Step 2: Write the failing parser tests** in
  `test_remote_protocol.c`, following the existing vector-parser test style
  (same fixtures and request-building helpers). Cases, each asserting
  `DT_REMOTE_ERR_INVALID_VALUE` and no patch produced, except the accepts:

```c
// accept: {"class":"bands","y":[0.5,0.5,0.5,0.5,0.5,0.5]} → patch with
//   class_id DT_REMOTE_PARAMETER_BANDS, 6 double y values, x == NULL
static void test_semantic_band_entry_parses(void **state);
// accept: same plus "x":[0.0,0.2,0.4,0.6,0.8,1.0] → x kept as 6 doubles
static void test_semantic_band_entry_with_x_parses(void **state);
// reject: y member missing / not an array
static void test_semantic_band_requires_y_array(void **state);
// reject: y with non-numeric, boolean, NaN/Infinity, null element
static void test_semantic_band_rejects_non_finite_samples(void **state);
// reject: y with 9 samples (> DT_REMOTE_BAND_WIRE_SAMPLE_CAP); y empty
static void test_semantic_band_rejects_oversized_sample_list(void **state);
// reject: x present with length != y length
static void test_semantic_band_rejects_mismatched_x_length(void **state);
// reject: unknown member {"class":"bands","y":[...],"points":[...]}
static void test_semantic_band_rejects_unknown_members(void **state);
// double-domain proof: y [0.50000001] survives as double
static void test_semantic_band_preserves_double_precision(void **state);
```

- [ ] **Step 3: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_protocol`
  Expected: FAIL (`"unsupported class 'bands'"` path today).
- [ ] **Step 4: Implement.** Union arm `dt_remote_band_patch_t bands;` in
  `dt_remote_semantic_patch_t`; free-function case (frees `name`, unrefs
  `y`, unrefs `x` if non-NULL). Parser: add the `"bands"` branch beside the
  `"vector"` branch, mirroring its hygiene — allowed members `class`, `y`,
  `x` only; `y` required JSON array, length in
  `[1, DT_REMOTE_BAND_WIRE_SAMPLE_CAP]`, every element through
  `_node_to_finite_double`; `x` optional, same element rules, and its
  length must equal `y`'s. Duplicate-ID detection across classes already
  runs before the class switch — verify with the existing cross-class test
  pattern, don't duplicate the mechanism.
- [ ] **Step 5: Run tests to verify pass.**
  Run: `ctest --test-dir build -R remote_protocol` — PASS; then full
  `ctest --test-dir build` — PASS.
- [ ] **Step 6: Commit** — `remote_protocol: parse bands-class semantic values (y required, x optional, doubles)`

### Task 3: Band engine and registry core

Spec §Engine and registry, §Class model. Mirrors the vector split:
`remote_band.c` = engine, `remote_band_registry.c` = static data. Reuses
`dt_remote_path_resolve` and the predicate/condition machinery verbatim.

**Files:**
- Create: `src/control/remote_band.h`, `src/control/remote_band.c`
- Create: `src/control/remote_band_registry.c` (registry infrastructure +
  empty adapter table; Tasks 5–7 fill it)
- Modify: the CMake source list that builds `remote_vector_registry.c`
- Create: `src/tests/unittests/control/test_remote_band.c`
- Modify: `src/tests/unittests/control/CMakeLists.txt` (mirror the
  `test_remote_vector` registration)

**Interfaces (Produces):**

```c
/* remote_band.h */
typedef enum dt_remote_band_x_policy_t
{ DT_REMOTE_BAND_X_FIXED = 0, DT_REMOTE_BAND_X_INTERIOR } dt_remote_band_x_policy_t;

typedef struct dt_remote_band_descriptor_t
{
  const char *name;               // stable semantic ID, e.g. "bands.luma"
  const char *display_name;
  const char *description;
  dt_remote_introspection_path_t native_x;  // → float array leaf (FIELD [+ INDEX row])
  dt_remote_introspection_path_t native_y;  // same shape as native_x
  guint count;                              // fixed band count N
  double y_minimum, y_maximum;              // [0,1] for all v1 adapters
  dt_remote_band_x_policy_t x_policy;
  double minimum_gap;                       // absolute, at-least; INTERIOR only (0.001)
  const char *x_shared_with;                // twin semantic ID, NULL if none
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_band_descriptor_t;

struct dt_remote_band_context_t;
typedef struct dt_remote_band_module_adapter_t
{
  const char *operation;
  guint minimum_params_version, maximum_params_version;
  const dt_remote_band_descriptor_t *bands;
  guint band_count;
  const char *const *prepare_fields;
  guint prepare_field_count;
  gboolean (*validate_completed)(const struct dt_remote_band_context_t *ctx,
                                 const void *new_params, dt_remote_error_t **error);
} dt_remote_band_module_adapter_t;

const dt_remote_band_module_adapter_t *
dt_remote_band_registry_lookup(const char *operation, guint params_version);
gboolean dt_remote_band_registry_validate(const dt_remote_band_module_adapter_t *adapter,
                                          const struct dt_iop_module_so_t *so,
                                          dt_remote_error_t **error);
gboolean dt_remote_band_list_schema(const struct dt_iop_module_so_t *so,
                                    GPtrArray **out_fields,   // dt_remote_band_schema_t*
                                    dt_remote_error_t **error);
gboolean dt_remote_band_read_values(const struct dt_iop_module_t *module,
                                    const void *params,
                                    GHashTable **out,         // name → dt_remote_band_value_t*
                                    dt_remote_error_t **error);
gboolean dt_remote_band_apply_patch(const struct dt_iop_module_t *module,
                                    const void *old_params, void *new_params,
                                    const dt_remote_patch_t *patch,
                                    dt_remote_error_t **error);
/* thin wrapper mirroring dt_remote_vector_apply_patch: null checks,
   own-class partition into a borrowed slice, prepare_needed gate over
   patch->scalar_values vs adapter prepare_fields, then apply_entries */
gboolean dt_remote_band_apply_entries(const struct dt_iop_module_t *module,
                                      const void *old_params, void *new_params,
                                      GPtrArray *entries,
                                      dt_remote_error_t **error);
gboolean dt_remote_band_validate(const dt_remote_band_descriptor_t *desc,
                                 const GArray *y /* double */,
                                 const GArray *x /* double, nullable */,
                                 const float *stored_x /* count floats, endpoint pinning */,
                                 dt_remote_error_t **error);  // pure
```

```c
/* remote_parameters.h — schema/value types, mirroring the vector ones */
typedef struct dt_remote_band_schema_t
{
  char *name; char *display_name; char *description;   // owned
  guint count;
  double y_minimum, y_maximum;
  dt_remote_band_x_policy_t x_policy;
  double minimum_gap;                                  // 0.0 = none
  char *x_shared_with;                                 // owned, nullable
  GArray *x;                                           // double, current positions
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;        // owned, nullable
  dt_remote_parameter_condition_t *writable_when;      // owned, nullable
} dt_remote_band_schema_t;
void dt_remote_band_schema_free(dt_remote_band_schema_t *schema);

typedef struct dt_remote_band_value_t
{
  char *name; GArray *y; GArray *x;   // double
  gboolean active, effective, writable_now;
} dt_remote_band_value_t;
void dt_remote_band_value_free(dt_remote_band_value_t *value);
```

The semantic schema/value wrappers gain `bands` union arms and
`dt_remote_semantic_schema_wrap_band` / `dt_remote_semantic_value_wrap_band`.

**Behavioral requirements** (each is a unit test in Step 1):

1. `registry_validate` fails closed on: `native_x` or `native_y` not
   resolving to a `float` array leaf of length exactly `count`; duplicate
   IDs within the adapter; an ID also registered by the curve or vector
   registry for the same op (cross-class uniqueness — call both other
   registries' lookups and compare names); `x_shared_with` naming a
   descriptor that does not exist in the same adapter, whose count
   differs, or whose `x_shared_with` does not point back (twin symmetry);
   INTERIOR policy with `minimum_gap <= 0`.
2. `list_schema` converts descriptors in registry order; ops with no
   adapter yield `*out_fields = NULL`, return TRUE (silent degrade).
3. `read_values` resolves both native paths against the live blob, widens
   floats to doubles into `y`/`x`, and stamps `active`/`writable_now` from
   the predicates (copy the vector engine's evaluation).
4. `apply_entries`: unknown ID → `DT_REMOTE_ERR_UNKNOWN_FIELD`; y length ≠
   `count` → `DT_REMOTE_ERR_INVALID_VALUE`; x present under FIXED policy →
   `DT_REMOTE_ERR_UNSUPPORTED_FIELD`; `dt_remote_band_validate` in double
   domain; **twin conflict**: two entries in one slice carrying x for the
   same twin group with any differing component (exact double compare) →
   `DT_REMOTE_ERR_INVALID_VALUE`; writability predicate evaluated against
   `new_params` after prepare-field composition (vector engine ordering);
   narrowing writes floats to `native_y` (and `native_x` when x given, plus
   the twin descriptor's `native_x` — same values); every other blob byte
   unchanged; `validate_completed` last, rolled back by the caller's temp
   blob on failure.
5. `dt_remote_band_validate` is pure and directly testable:
   - y: reject length ≠ count, non-finite (defense in depth), below
     `y_minimum` / above `y_maximum` (test at the double boundary:
     max 1.0, value 1.00000001 → reject);
   - x under INTERIOR: reject length ≠ count; endpoint mismatch
     (`(float)x[0] != stored_x[0]` or `(float)x[count−1] != stored_x[count−1]`);
     not strictly ascending; adjacent gap `< 0.001`; accept gap `== 0.001`
     and endpoints exactly equal to stored.

- [ ] **Step 1: Write failing unit tests** in `test_remote_band.c` for
  requirements 1–5, using a test-local static adapter against a real loaded
  module `.so` exactly as `test_remote_vector.c` does (reuse its
  module-loading fixture). Use `lowlight` as the fixture op for the generic
  tests (simplest: 1-D `transition_x`/`transition_y[6]` fields) with a
  hand-rolled descriptor, plus a second hand-rolled INTERIOR-policy
  descriptor pair against `atrous`'s 2-D `x`/`y` arrays for the x-policy
  and twin tests — the engine is proven before any shipped adapter exists.
  Register the binary in `src/tests/unittests/control/CMakeLists.txt` by
  copying the `test_remote_vector` block.
- [ ] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_band`
  Expected: FAIL (nothing implemented).
- [ ] **Step 3: Implement** `remote_band.h/.c`, `remote_band_registry.c`
  (empty `s_adapters[]` + lookup + validate), the schema/value types +
  frees in `remote_parameters.h/.c`, and the wrapper band arms. Copy
  structure from the vector twins; do not invent new patterns. Add both
  `.c` files to the CMake source list.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_band: sampled-response engine and registry core`

### Task 4: Bands into the dispatch table; wire serializers; capability

Spec §Wire and MCP contract. After this task, any registered band adapter
is fully live end-to-end.

**Files:**
- Modify: `src/control/remote_edit.c` — add the bands row to
  `s_class_ops[]` (order: curve, vector, bands) and the bands registry to
  the `represented_by` annotation loop
- Modify: `src/control/remote_protocol.c` — serializers + hello capability
  (find the capability array: `grep -n "vector_params" src/control/remote_protocol.c`)
- Test: `src/tests/unittests/control/test_remote_edit.c`,
  `test_remote_protocol.c`

**Interfaces (Consumes):** everything Tasks 1–3 produced.

- [ ] **Step 1: Write failing tests.**
  - `test_remote_protocol.c`: hello advertises `"band_params"` alongside
    `curve_params`/`vector_params` (copy the existing capability test).
  - Serializer tests (follow the vector serializer test style): a band
    schema serializes with `class:"bands"`, `count`, `y_range`
    `{minimum, maximum}`, `x_policy:"fixed"|"interior"`, `min_gap` and
    `x_shared_with` present only when applicable, current `x` array; a band
    value serializes `{y:[...], x:[...], active, effective, writable_now}`.
  - `test_remote_edit.c`: with a test-local band adapter registered,
    `get_module_schema` includes the band field and marks the native
    `x`/`y` array fields `writable:false` with `represented_by`; a mutation
    with a band entry applies through the dispatch table and reads back; a
    mixed request (scalar + curve + vector + band where the fixture module
    allows, else scalar + band) applies atomically and a failing band entry
    rejects the whole request byte-atomically.
- [ ] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R "remote_protocol|remote_edit"`
  Expected: FAIL.
- [ ] **Step 3: Implement.** `_band_schema_to_json` / `_band_value_to_json`
  beside the vector serializers; new wrapper-tag switch arms; the
  `band_params` capability string; the `s_class_ops[]` bands row
  (`dt_remote_band_list_schema`, `dt_remote_band_read_values`,
  `dt_remote_band_apply_patch`, band wrap helpers); bands registry in the
  `represented_by` annotation (both `native_x` and `native_y` leaves point
  at the semantic name).
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_edit/protocol: bands class live end-to-end (dispatch row, serializers, band_params)`

### Task 5: lowlight and rawdenoise adapters — rawdenoise fixed x, lowlight interior x

Spec §Adapters. The two simple adapters prove 1-D and 2-D native paths.
(Amended post-Task-6: lowlight is interior-x, per the resolved-decisions
bullet above; rawdenoise stays fixed.)

**Files:**
- Modify: `src/control/remote_band_registry.c`
- Test: `src/tests/unittests/control/test_remote_band.c`

**Descriptor data (normative):**

| op | name | native rows (x, y) | N |
|---|---|---|---|
| `lowlight` v1 | `bands.transition` | `transition_x` / `transition_y` (1-D) | 6 |
| `rawdenoise` v2 | `bands.all` | `x`/`y` row 0 | 5 |
| | `bands.red` | row 1 | 5 |
| | `bands.green` | row 2 | 5 |
| | `bands.blue` | row 3 | 5 |

All: y range [0,1], no twins, no predicates, no `prepare_fields`, no
`validate_completed`. rawdenoise: `DT_REMOTE_BAND_X_FIXED`. lowlight:
`DT_REMOTE_BAND_X_INTERIOR`, `minimum_gap` 0.001 (amended). Versions
pinned lowlight min==max==1, rawdenoise min==max==2.

- [ ] **Step 1: Write failing per-adapter tests** (mirror the m4
  per-adapter test style — read the borders/watermark tests in
  `test_remote_vector.c` first): registry lookup by op+version succeeds,
  wrong version fails; `registry_validate` passes against the real loaded
  `.so`; schema lists the expected names/counts/policies; read-back of
  default params returns the module defaults (lowlight x = 0.0,0.2,…,1.0;
  y = 0.5 six times; rawdenoise x = k/4, y = 0.5); apply a valid y write
  and read it back; rawdenoise: x entry rejected with `unsupported_field`;
  lowlight (amended): a valid interior-x write round-trips, endpoint
  violation and below-minimum gap rejected with `invalid_value`; y out of
  range rejected; sibling scalar (`blueness` / `threshold`) unchanged by a
  band write.
- [ ] **Step 2: Run to verify failure.**
  Run: `ctest --test-dir build -R remote_band` — FAIL (no adapters).
- [ ] **Step 3: Implement both adapter tables** in
  `remote_band_registry.c` as static data, following the vector registry's
  layout exactly.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_band: lowlight and rawdenoise adapters (fixed x)`

### Task 6: denoiseprofile adapter — six channels, v12

Spec §Adapters. Note: all six channels always writable (the GUI stores all
channels regardless of the active wavelet color mode); the noise-fit
`a[3]`/`b[3]` arrays stay excluded (already denylisted since milestone 2).

**Files:**
- Modify: `src/control/remote_band_registry.c`
- Test: `src/tests/unittests/control/test_remote_band.c`

**Descriptor data (normative):** op `denoiseprofile`, version pinned
min==max==12, `x`/`y` rows: `bands.all` 0, `bands.red` 1, `bands.green` 2,
`bands.blue` 3, `bands.y0` 4, `bands.u0v0` 5; N = 7; y range [0,1];
`DT_REMOTE_BAND_X_FIXED`; no twins/predicates/prepare/validate_completed.

- [ ] **Step 1: Write failing per-adapter tests**: lookup (v12 yes, v11
  no), registry_validate against the real `.so`, schema names and count 7,
  read-back of defaults (x = b/6 for b in 0..6, y = 0.5), y write on
  `bands.u0v0` round-trips while rows 0–4 and all scalars are byte-
  unchanged, x rejected, count-mismatch (6 samples) rejected.
- [ ] **Step 2: Run to verify failure.** `ctest --test-dir build -R remote_band` — FAIL.
- [ ] **Step 3: Implement the adapter table.**
- [ ] **Step 4: Run tests to verify pass.** `ctest --test-dir build` — PASS.
- [ ] **Step 5: Commit** — `remote_band: denoiseprofile adapter (six channels, v12)`

### Task 7: atrous adapter — interior x, twin mirroring, octaves denylist

Spec §Adapters, §Class model (twin-channel x sharing). The hard adapter.

**Files:**
- Modify: `src/control/remote_band_registry.c`
- Modify: `src/control/remote_edit.c` — denylist table (the
  `s_op_denylists[]` array): insert `{ "atrous", DENY("octaves") }` after
  the `ashift` row (alphabetical)
- Test: `src/tests/unittests/control/test_remote_band.c`,
  `test_remote_edit.c` (denylist)

**Descriptor data (normative):** op `atrous`, version pinned min==max==2,
`x`/`y` rows (enum `atrous_channel_t`, atrous.c:64):

| name | row | x policy | x_shared_with |
|---|---|---|---|
| `bands.luma` | 0 (`atrous_L`) | INTERIOR, min_gap 0.001 | `bands.luma_threshold` |
| `bands.chroma` | 1 (`atrous_c`) | INTERIOR, min_gap 0.001 | `bands.chroma_threshold` |
| `bands.sharpness` | 2 (`atrous_s`) | INTERIOR, min_gap 0.001 | NULL |
| `bands.luma_threshold` | 3 (`atrous_Lt`) | INTERIOR, min_gap 0.001 | `bands.luma` |
| `bands.chroma_threshold` | 4 (`atrous_ct`) | INTERIOR, min_gap 0.001 | `bands.chroma` |

N = 6; y range [0,1]; no predicates/prepare/validate_completed. `mix` stays
an ordinary writable scalar (no interaction with the adapter).

- [ ] **Step 1: Write failing tests.**
  - Per-adapter basics as in Tasks 5–6 (lookup v2, validate, schema — now
    asserting `x_policy:"interior"`, `min_gap`, and the four
    `x_shared_with` links; defaults x = k/5, y = 0.5).
  - y-only write on `bands.luma` round-trips; `bands.luma_threshold` y
    unchanged (y is NOT shared, only x).
  - x write on `bands.luma` (e.g. interior nodes shifted:
    `[0.0, 0.15, 0.4, 0.6, 0.85, 1.0]`) applies to row 0 AND mirrors to
    row 3's native x; rows 1/2/4 x unchanged.
  - x write on `bands.sharpness` mirrors nowhere.
  - endpoint violation (`x[0] = 0.01`) rejected; descending x rejected;
    gap 0.0005 rejected; gap exactly 0.001 accepted.
  - twin conflict: one request with x on both `bands.luma` and
    `bands.luma_threshold` differing in one component → rejected, blob
    byte-identical; same x on both → accepted, applied once.
  - denylist (in `test_remote_edit.c`, copy an existing denylist test):
    atrous schema marks `octaves` `writable:false`; a scalar write to
    `octaves` is rejected.
- [ ] **Step 2: Run to verify failure.**
  Run: `ctest --test-dir build -R "remote_band|remote_edit"` — FAIL.
- [ ] **Step 3: Implement** the adapter table and the denylist row.
- [ ] **Step 4: Run tests to verify pass.** `ctest --test-dir build` — PASS.
- [ ] **Step 5: Commit** — `remote_band: atrous adapter (interior x, twin mirroring); deny octaves`

### Task 8: Python sidecar — `bands` argument and error-mapping cleanup

Spec §Sidecar. Mirrors the m4 `vectors` task, plus ride-along 2.

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py`, `tools/mcp/tests/test_protocol.py`
  (follow where the existing `vectors` tests live —
  `grep -rn "_wire_vector_values\|vectors=" tools/mcp/tests/`)

**Interfaces (Produces):**

```python
def _wire_band_values(bands: dict[str, Any]) -> dict[str, Any]:
    """Translate the `bands` tool argument to wire semantic_values
    entries. Tool shape: {name: {"y": [...], "x": [...]?}}. Mirrors
    _wire_vector_values: validate fully before any wire traffic, raise
    ToolError on bad shapes."""
```

Wire entry produced: `{"class": "bands", "y": [floats], "x": [floats]?}`.

- [ ] **Step 1: Write failing sidecar tests.**
  - `_wire_band_values` accepts `{"bands.luma": {"y": [0.5]*6}}` and
    `{"y": [...], "x": [...]}`; rejects (ToolError): missing `y`, empty
    `y`, non-finite/boolean elements, `x` length ≠ `y` length, unknown keys
    in the spec dict, non-dict spec.
  - `set_module_params(bands=...)` merges band entries into
    `semantic_values` beside curves and vectors; overlapping semantic IDs
    across any two of the three arguments raise ToolError (extend the
    existing curves/vectors overlap test to the three-way case).
  - Capability gate: bands given but `band_params` absent →
    `TransportError` mentioning `band_params` (copy the `vector_params`
    gate test).
  - **Cleanup (ride-along 2):** `_wire_semantic_values` with an unknown
    interpolation now raises `ToolError` (not `ValueError`) — update the
    existing covering test to assert ToolError; add an assertion that no
    tool code path raises bare ValueError for caller input
    (`grep "raise ValueError" src/darktable_mcp/server.py` returns nothing
    after this task — enforce by changing the raise, not by a meta-test).
- [ ] **Step 2: Run to verify failure.**
  Run: `cd tools/mcp && .venv/bin/pytest -q` — FAIL.
- [ ] **Step 3: Implement.** `_wire_band_values` (validation shape above);
  `bands: dict | None = None` parameter on `set_module_params` wired
  through the same merge/overlap/capability pattern as `vectors` (three-way
  overlap check: any ID in more than one of curves/vectors/bands →
  ToolError listing the IDs); change `_wire_semantic_values`'s
  `raise ValueError` to `raise ToolError` and update its docstring
  sentence to say ToolError.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `cd tools/mcp && .venv/bin/pytest -q` — PASS.
- [ ] **Step 5: Commit** — `mcp sidecar: bands argument; caller-input errors are ToolError uniformly`

### Task 9: Live integration gates

Spec §Testing (integration). One file, mirroring
`test_vectors_milestone4.py`'s structure and helpers (read it first; reuse
the harness fixtures).

**Files:**
- Create: `tools/mcp/tests/integration/test_bands_milestone5.py`

**Gates** (each is one test):

1. hello advertises `band_params`.
2. `get_module_schema("atrous")` lists the five band fields with
   `x_policy:"interior"`, `min_gap` 0.001, `x_shared_with` links; native
   `x`/`y` marked `writable:false` with `represented_by`; `octaves`
   `writable:false`.
3. `get_module_schema("denoiseprofile")` lists six `x_policy:"fixed"`
   fields, count 7.
4. lowlight y write round-trips via `get_module_params`; a lowlight
   interior-x write (endpoints pinned) round-trips too (amended).
5. rawdenoise `bands.green` y write round-trips; `threshold` scalar
   unchanged.
6. denoiseprofile `bands.u0v0` y write round-trips.
7. atrous y write on `bands.luma` round-trips; `bands.luma_threshold` y
   unchanged.
8. atrous x write on `bands.luma` round-trips AND `bands.luma_threshold`
   reads back the same x (live twin mirroring).
9. atrous twin-conflict request rejected with `invalid_value`; both
   channels read back unchanged.
10. y out of range rejected with `invalid_value` carrying `band_index`;
    x on a fixed-policy module rejected with `unsupported_field`.
11. mixed request: atrous `mix` scalar + `bands.luma` y in one call → one
    history item (history length check, curve-test precedent).
12. undo after a band write restores the previous band values (copy the
    vector undo gate's structure).
13. stale-revision band write rejected (copy the vector stale gate).
14. effectiveness fixture: enable atrous, apply a strong `bands.luma`
    lift (y = [1.0]*6 vs default 0.5), render_preview differs from the
    default render (pixel-difference helper from the m4 file).

- [ ] **Step 1: Write all gates** (they fail against the pre-Task-4 binary
  only; against the finished branch they should pass — this task runs last
  precisely because it needs the full stack).
- [ ] **Step 2: Run the integration suite.**
  Run: `cd tools/mcp && DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`
  Expected: all gates PASS (plus the pre-existing OpenCL skip).
- [ ] **Step 3: Run everything.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build && cd tools/mcp && .venv/bin/pytest -q`
  Expected: all PASS.
- [ ] **Step 4: Commit** — `mcp tests: milestone 5 live integration gates (bands)`

### Task 10: Documentation

Spec §Documentation — same milestone, not after.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `tools/mcp/README.md`, `tools/mcp/docs/remote-control.md`
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-milestone5-bands-class-design.md` (status line → implemented)

Line numbers below are as of the plan's writing; match by content if they
have drifted.

- [ ] **Step 1: supported-operations.** Milestone table: add the m5 row
  (bands class, four modules, both ride-alongs). Move `atrous` from Tier 3
  to Tier 1 and `denoiseprofile`, `rawdenoise`, `lowlight` from Tier 2 to
  Tier 1 — each row gets `since` = m5, keeps its usage rating, and
  describes its semantic names in the milestone-4 rows' phrasing
  ("**milestone 5, semantic bands**: … via `semantic_values`
  (`band_params` capability); native arrays stay read-only with
  `represented_by`"). Update tier counts: Tier 1 50 → 54, Tier 2 14 → 11,
  Tier 3 7 → 6. Remove the Tier-3 preamble sentence that names `atrous` as
  out of the curve pattern's reach (it is now covered). Appendix: add
  `| atrous | octaves | auto-derived from image size in commit_params |`
  (keep the existing ashift row's alphabetical position). Update the
  "after this milestone" framing sentence in "How support is determined"
  to mention the bands class.
- [ ] **Step 2: protocol-reference.** Add the `band_params` capability
  section beside `vector_params`: advertises writable bands-class semantic
  parameters, never advertised without accepting band entries. Copy the
  normative schema/value/patch JSON shapes and vocabulary from spec §Wire
  and MCP contract (`count`, `y_range`, `x_policy` `fixed`/`interior`,
  `min_gap`, `x_shared_with`, `band_index`, `at_least`), **including
  `"class": "bands"` in the patch example** (the m4 final review caught
  exactly this omission — do not repeat it). Add the error-precedence
  paragraph: parse errors first, then engine dispatch order curve →
  vector → bands.
- [ ] **Step 3: MCP user docs.** README + remote-control.md: `bands`
  argument with an atrous example (y lift plus an x write, noting the twin
  mirroring side effect on `bands.luma_threshold`) and a denoiseprofile
  y-only example; the `bands` bullet goes after the `vectors` bullet in
  remote-control.md.
- [ ] **Step 4: Verify all suites one final time** (full ctest + pytest +
  integration) and commit —
  `darktable-mcp: milestone 5 docs — tiers, bands protocol, sidecar usage`

---

## Self-review notes (already applied)

- Spec coverage: composed seam (T1), wire/parser (T2), engine/registry +
  twin/x-policy model (T3), capability/serializers/dispatch row (T4),
  adapters (T5–T7), octaves denylist (T7), sidecar + error cleanup (T8),
  integration incl. undo/stale/effectiveness (T9), docs incl. error
  precedence (T10).
- Type consistency: `dt_remote_band_*` names used identically across Tasks
  2–8; `apply_entries` signature identical across the three engines
  (Task 1 defines it, Tasks 3–4 conform).
- The 16-name count (5+6+4+1) is asserted across Tasks 5–7's schema tests.
- Deliberately not planned: no `bands` support in `reset_module` beyond
  what falls out of module reset (whole-params reset already covers it);
  no per-band partial writes (whole-set replacement is normative).
