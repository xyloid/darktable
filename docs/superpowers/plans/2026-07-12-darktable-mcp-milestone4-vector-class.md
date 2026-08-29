# darktable MCP Milestone 4 — Semantic Vector Class — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the semantic vector class (subtypes `vector`/`color`/`levels`)
and five module adapters — colorbalance, channelmixerrgb, rgblevels, borders,
watermark — per the approved design
`docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-vector-class-design.md`
(revision `89291ae3e6`). That spec is normative; where this plan says "per
spec §X" the implementer must read that section.

**Architecture:** Milestone 2/3 built a curve-only pipeline: parser
(`remote_protocol.c`) → tagged patch envelope (`remote_parameters.h`) →
engine (`remote_curve.c`: path resolution, validation, apply) → registry
(`remote_curve_registry.c`: static descriptors + callbacks) → four seams in
`remote_edit.c` → JSON serializers → Python sidecar. Milestone 4 first makes
the schema/value containers class-tagged (curve-only, wire byte-identical),
then adds a parallel vector engine + registry reusing the existing generic
pieces (`dt_remote_path_resolve`, predicates, `prepare_fields` composition),
generalizes the four seams to an explicit two-call sequence over both
registries, and ships five static adapters. Doubles end-to-end until range
validation; complete-replacement writes; reject-don't-clamp.

**Tech Stack:** C (GLib, JSON-GLib, darktable introspection), cmocka unit
tests under `src/tests/unittests/control/`, Python 3 + FastMCP sidecar in
`tools/mcp/`, pytest integration harness under Xvfb.

## Global Constraints

- Work in the git worktree `<REPO>/.claude/worktrees/mcp-remote-edit`, branch `worktree-mcp-remote-edit`. All paths below are relative to that root.
- TDD: every behavior change lands with its failing test first. Pure refactors (Task 1) are covered by the existing suites staying green.
- Commit messages end with the two trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_013WEQMKDwcviZRWSK2ugzTH`.
- Verification at every commit: `cmake --build build -j$(nproc)` then
  `ctest --test-dir build` (all suites), `cd tools/mcp && .venv/bin/pytest -q`;
  for Task 10 also `DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`.
- **Curve behavior byte-for-byte unchanged.** Existing curve suites are the
  regression net. If a curve test fails, the change is wrong — stop.
- Params versions (verified from `DT_MODULE_INTROSPECTION`): `colorbalance` = 3,
  `channelmixerrgb` = 3, `rgblevels` = 1, `borders` = 4, `watermark` = 7.
  Every adapter pins `minimum_params_version == maximum_params_version` to
  exactly these.
- Stable wire vocabulary (spec §Wire and MCP contract): class `"vector"`;
  subtypes `"vector"`, `"color"`, `"levels"`; color space `"display_rgb"`;
  capability `"vector_params"`; gap comparison `"at_least"`.
- Vector components travel as **doubles** (`GArray` of `double`) from parser
  through range validation; narrow to `float` only when writing the blob.
- Wire patch entries require `"class": "vector"` — the parser's existing
  "class is required in mutation values" rule (remote_protocol.c:1099)
  applies to vectors too; the spec's patch sketch omits it, the parser rule
  wins.
- rgblevels ordering: `black < grey < white` per triple with **absolute
  minimum gap `FLT_EPSILON`, at-least comparison** (spec rationale: finite
  `1/(white−black)`, picker round-trip safety).

## Decisions resolved from the spec's "Open items"

- **channelmixerrgb fourth component: reserved.** `commit_params`
  (src/iop/channelmixerrgb.c:3083) reads only `[0..2]` of each row and
  explicitly zeroes the pipeline's fourth elements (:3102–3104). Adapters
  expose 3 components per row and preserve `[3]` verbatim (basecurve
  reserved-channel precedent).
- **borders/watermark color ranges: descriptor-carried 0.0–1.0 is
  normative.** The struct fields carry no `$MIN/$MAX`; descriptor ranges are
  the validation source, exactly as curve axis ranges are descriptor-owned.
- **Generalized dispatch: explicit two-call sequence per seam** (curve call
  then vector call), not a class vtable — YAGNI with two classes; revisit
  when milestone 5 adds a third.
- **No `prepare` callback on the vector adapter struct.** No v1 vector
  adapter needs one (rgblevels explicitly must NOT reset). `prepare_fields`
  is kept: it lists scalar fields (colorbalance `mode`, rgblevels
  `autoscale`) whose same-patch writes must compose before writability
  evaluation, exactly as the curve engine does it. Add the callback pointer
  only when a module needs it.
- **Undo/history/stale-revision:** the apply path is shared, and curve gates
  already prove the machinery (tools/mcp/tests/integration/test_curves.py:178
  undo, :307 stale revision). Task 10 adds one vector undo gate and one
  vector stale-revision gate to prove the generalized dispatch preserves it;
  everything else is referenced, not duplicated.
- **Watermark effectiveness fixture:** strings are not writable over MCP, so
  the live gate seeds `filename = "simple-text.svg"` through an `.xmp`
  sidecar placed next to the copied test image before darktable launches
  (harness copies the image at tools/mcp/tests/integration/harness.py:326;
  darktable reads sidecars on import). Task 10 builds the params blob with
  `struct.pack` from the verified v7 layout.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/control/remote_parameters.h/.c` | modify | class enum cleanup; vector patch/schema/value types; tagged semantic schema/value wrappers + free functions |
| `src/control/remote_protocol.c` | modify | parser branch for class `"vector"`; `_vector_schema_to_json` / `_vector_value_to_json`; serializer switch on wrapper tag; `vector_params` in hello (:583) |
| `src/control/remote_vector.h/.c` | create | vector engine: descriptor/adapter types, `list_schema`, `read_values`, `apply_patch`, pure validator |
| `src/control/remote_vector_registry.c` | create | static adapters for the five modules; `registry_lookup`, `registry_validate` (incl. cross-class ID uniqueness) |
| `src/control/remote_edit.h/.c` | modify | containers hold tagged wrappers; four seams become two-call sequences |
| `src/tests/unittests/control/test_remote_vector.c` | create | engine + per-adapter unit tests |
| `src/tests/unittests/control/test_remote_protocol.c` | modify | vector parser hygiene tests |
| `src/tests/unittests/control/CMakeLists.txt` | modify | register `test_remote_vector` |
| `tools/mcp/src/darktable_mcp/server.py` | modify | `vectors` argument, `_wire_vector_values`, `vector_params` gate |
| `tools/mcp/tests/test_tools.py`, `tests/test_protocol.py` | modify | sidecar unit tests |
| `tools/mcp/tests/integration/test_vectors_milestone4.py` | create | live gates + XMP fixture helper |
| docs (see Task 11) | modify | tiers, appendix, protocol reference, README/docstrings |

`src/CMakeLists.txt` (or the list that compiles `src/control/remote_*.c` —
find it with `grep -rn "remote_curve_registry" src/CMakeLists.txt
src/control/CMakeLists.txt`) gains the two new C files in Task 3.

---

### Task 1: Tagged semantic schema/value containers (curve-only refactor)

Pure refactor, wire byte-identical; the existing suites are the proof. This
implements spec §Protocol and engine changes item 5 for the curve arm only.

**Files:**
- Modify: `src/control/remote_parameters.h` (new wrapper types + free fns)
- Modify: `src/control/remote_parameters.c`
- Modify: `src/control/remote_edit.h:209` (`semantic_fields` comment/type), `:268` (`semantic_values`)
- Modify: `src/control/remote_edit.c` (wrap at production sites)
- Modify: `src/control/remote_protocol.c:766,882` (unwrap at serialization sites)

**Interfaces (Produces):**

```c
/* remote_parameters.h — after dt_remote_semantic_patch_t */
typedef struct dt_remote_semantic_schema_t
{
  dt_remote_parameter_class_t class_id;
  union { dt_remote_curve_schema_t *curve; } u;   // owned
} dt_remote_semantic_schema_t;

typedef struct dt_remote_semantic_value_t
{
  dt_remote_parameter_class_t class_id;
  union { dt_remote_curve_value_t *curve; } u;    // owned
} dt_remote_semantic_value_t;

dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_curve(dt_remote_curve_schema_t *s);
dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_curve(dt_remote_curve_value_t *v);
void dt_remote_semantic_schema_free(gpointer schema_ptr);  // GDestroyNotify-able
void dt_remote_semantic_value_free(gpointer value_ptr);
```

- [x] **Step 1: Add the wrapper types and free functions.** Free functions
  switch on `class_id` and delegate to `dt_remote_curve_schema_free` /
  `dt_remote_curve_value_free`; `default:` frees only the wrapper. Wrap
  helpers `g_malloc0` the wrapper, set the tag, take ownership.
- [x] **Step 2: Retype the containers.** `dt_remote_module_schema_t.semantic_fields`
  now holds `dt_remote_semantic_schema_t *`;
  `dt_remote_mutation_result_t.semantic_values` maps name →
  `dt_remote_semantic_value_t *`. Update both comments and the container
  free functions to use the new destructors.
- [x] **Step 3: Wrap at the producers.** In `remote_edit.c`, everywhere
  `dt_remote_curve_list_schema` / `dt_remote_curve_read_values` output is
  inserted into these containers, wrap with the helpers. (Simplest: keep
  those two curve functions returning curve types and wrap at the three
  insertion sites in `remote_edit.c` — `grep -n "semantic_fields\|semantic_values"
  src/control/remote_edit.c` lists them.)
- [x] **Step 4: Unwrap at the serializers.** At `remote_protocol.c:766` and
  `:882`, switch on the wrapper tag; the `DT_REMOTE_PARAMETER_CURVE` arm
  calls the existing `_curve_schema_to_json(w->u.curve)` /
  `_curve_value_to_json(w->u.curve)`; any other tag is
  `g_assert_not_reached()` for now (Task 4 replaces it).
- [x] **Step 5: Build and verify byte-identical wire.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build && cd tools/mcp && .venv/bin/pytest -q`
  Expected: all pass, zero test edits.
- [x] **Step 6: Commit** — `remote_parameters: class-tagged semantic schema/value wrappers (curve-only)`

### Task 2: Vector class core — enum cleanup, patch arm, parser branch

Spec §Protocol and engine changes items 1–2 and §Why one class (enum
cleanup).

**Files:**
- Modify: `src/control/remote_parameters.h/.c`
- Modify: `src/control/remote_protocol.c` (parser, around :1113)
- Test: `src/tests/unittests/control/test_remote_protocol.c`

**Interfaces (Produces):**

```c
/* remote_parameters.h */
typedef enum dt_remote_parameter_class_t
{
  DT_REMOTE_PARAMETER_CURVE = 1,
  DT_REMOTE_PARAMETER_SAMPLED_RESPONSE,   // reserved, milestone 5
  DT_REMOTE_PARAMETER_VECTOR              // replaces DT_REMOTE_PARAMETER_LEVELS
} dt_remote_parameter_class_t;

typedef struct dt_remote_vector_patch_t
{
  char *name;        // owned semantic ID
  GArray *values;    // double elements, owned
} dt_remote_vector_patch_t;
/* added to dt_remote_semantic_patch_t's union as `vector` */

/* remote_protocol.c */
#define DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP 8
```

- [x] **Step 1: Write the failing parser tests** in
  `test_remote_protocol.c`, following the file's existing curve-parser test
  style (same fixtures, same request-building helpers — read the
  `semantic value` tests there first). Cases, each asserting
  `DT_REMOTE_ERR_INVALID_VALUE` and that nothing was produced, except the
  accept case:

```c
// accept: {"class":"vector","values":[1.0,1.1,1.0,0.95]} → patch with
//   class_id DT_REMOTE_PARAMETER_VECTOR, 4 double values, name kept
static void test_semantic_vector_entry_parses(void **state);
// reject: values member missing / not an array
static void test_semantic_vector_requires_values_array(void **state);
// reject: non-numeric ("x"), boolean, NaN/Infinity spellings, null element
static void test_semantic_vector_rejects_non_finite_components(void **state);
// reject: 9 components (> DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP)
static void test_semantic_vector_rejects_oversized_component_list(void **state);
// reject: unknown member {"class":"vector","values":[...],"points":[...]}
static void test_semantic_vector_rejects_unknown_members(void **state);
// reject: duplicate semantic IDs across classes in one request
//   (two entries with the same name, one curve one vector)
static void test_semantic_duplicate_ids_rejected_across_classes(void **state);
// double-domain proof: values [2.00000001] survive as doubles — the parser
//   must NOT narrow; assert g_array_index(values, double, 0) > 2.0
static void test_semantic_vector_preserves_double_precision(void **state);
```

- [x] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_protocol`
  Expected: FAIL (new tests; `"unsupported class 'vector'"` path today).
- [x] **Step 3: Implement.**
  - Enum: remove `DT_REMOTE_PARAMETER_LEVELS`, add
    `DT_REMOTE_PARAMETER_VECTOR`; update the case list in
    `dt_remote_semantic_patch_free` (remote_parameters.c:59): the vector
    case frees `name` and unrefs `values`.
  - Union arm `dt_remote_vector_patch_t vector;` in
    `dt_remote_semantic_patch_t`.
  - Parser: at the class dispatch (remote_protocol.c:1113, the
    `g_strcmp0(class_name, "curve")` reject), branch on the class string.
    The vector branch mirrors the curve branch's hygiene: per-entry
    allowed-member check (`class`, `values` only — extend the same
    member-loop the curve branch uses), `values` must be a JSON array,
    length in `[1, DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP]`, every element
    through the existing `_node_to_finite_double`. Duplicate-ID detection
    already runs before the class switch (name-keyed) — verify it, don't
    duplicate it.

```c
    else if(!g_strcmp0(class_name, "vector"))
    {
      JsonNode *values_node = json_object_get_member(entry, "values");
      if(!values_node || !JSON_NODE_HOLDS_ARRAY(values_node))
      { entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
          _("semantic value '%s' requires a 'values' array"), name); break; }
      JsonArray *arr = json_node_get_array(values_node);
      const guint n = json_array_get_length(arr);
      if(n == 0 || n > DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP)
      { entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
          _("semantic value '%s' has %u components; the request limit is %d"),
          name, n, DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP); break; }
      GArray *values = g_array_sized_new(FALSE, FALSE, sizeof(double), n);
      for(guint i = 0; i < n && !entry_err; i++)
      {
        double v = 0.0;
        if(!_node_to_finite_double(json_array_get_element(arr, i), &v))
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
            _("component %u of semantic value '%s' must be a finite number"), i, name);
        else g_array_append_val(values, v);
      }
      if(entry_err) { g_array_unref(values); break; }
      dt_remote_semantic_patch_t *semantic = g_malloc0(sizeof(dt_remote_semantic_patch_t));
      semantic->class_id = DT_REMOTE_PARAMETER_VECTOR;
      semantic->value.vector.name = g_strdup(name);
      semantic->value.vector.values = values;
      g_ptr_array_add(patches, semantic);
    }
```

- [x] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build -R remote_protocol` — PASS; then full
  `ctest --test-dir build` — PASS (curve suites untouched).
- [x] **Step 5: Commit** — `remote_protocol: parse vector-class semantic values (doubles, capped, strict members)`

### Task 3: Vector engine and registry core

Spec §Protocol and engine changes item 3. Mirrors the curve split:
`remote_vector.c` = engine, `remote_vector_registry.c` = static data. Reuses
`dt_remote_path_resolve` (remote_curve.h:96) and the curve predicate/
condition machinery verbatim.

**Files:**
- Create: `src/control/remote_vector.h`, `src/control/remote_vector.c`
- Create: `src/control/remote_vector_registry.c` (registry infrastructure +
  empty adapter table; Tasks 5–8 fill it)
- Modify: the CMake source list that builds `remote_curve_registry.c`
- Create: `src/tests/unittests/control/test_remote_vector.c`
- Modify: `src/tests/unittests/control/CMakeLists.txt` (mirror the
  `test_remote_curve` registration)

**Interfaces (Produces):**

```c
/* remote_vector.h */
typedef enum dt_remote_vector_subtype_t
{ DT_REMOTE_VECTOR_PLAIN = 0, DT_REMOTE_VECTOR_COLOR, DT_REMOTE_VECTOR_LEVELS }
dt_remote_vector_subtype_t;

typedef struct dt_remote_vector_component_t
{ const char *name; double minimum; double maximum; } dt_remote_vector_component_t;

typedef struct dt_remote_vector_descriptor_t
{
  const char *name;               // stable semantic ID
  const char *display_name;
  const char *description;
  dt_remote_introspection_path_t native;  // → float array leaf (FIELD [+ INDEX row])
  guint component_count;                  // exposed on the wire
  const dt_remote_vector_component_t *components;  // component_count entries
  guint native_capacity;                  // full array length; [component_count..) preserved
  dt_remote_vector_subtype_t subtype;
  const char *color_space;                // non-NULL iff subtype COLOR ("display_rgb")
  gboolean strictly_increasing;           // subtype LEVELS ordering
  double minimum_gap;                     // absolute, at-least; LEVELS only
  const dt_remote_parameter_predicate_t *active_when;    // same type curve descriptors use
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_vector_descriptor_t;

struct dt_remote_vector_context_t;
typedef struct dt_remote_vector_module_adapter_t
{
  const char *operation;
  guint minimum_params_version, maximum_params_version;
  const dt_remote_vector_descriptor_t *vectors;
  guint vector_count;
  const char *const *prepare_fields;
  guint prepare_field_count;
  gboolean (*validate_completed)(const struct dt_remote_vector_context_t *ctx,
                                 const void *new_params, dt_remote_error_t **error);
} dt_remote_vector_module_adapter_t;

typedef struct dt_remote_vector_context_t
{
  const struct dt_iop_module_t *module;
  const dt_introspection_t *introspection;
  const dt_remote_vector_module_adapter_t *adapter;
} dt_remote_vector_context_t;

const dt_remote_vector_module_adapter_t *
dt_remote_vector_registry_lookup(const char *operation, guint params_version);
gboolean dt_remote_vector_registry_validate(const dt_remote_vector_module_adapter_t *adapter,
                                            const struct dt_iop_module_so_t *so,
                                            dt_remote_error_t **error);
gboolean dt_remote_vector_list_schema(const struct dt_iop_module_so_t *so,
                                      GPtrArray **out_fields,      // dt_remote_vector_schema_t*
                                      dt_remote_error_t **error);
gboolean dt_remote_vector_read_values(const struct dt_iop_module_t *module,
                                      const void *params,
                                      GHashTable **out,            // name → dt_remote_vector_value_t*
                                      dt_remote_error_t **error);
gboolean dt_remote_vector_apply_patch(const struct dt_iop_module_t *module,
                                      const void *old_params, void *new_params,
                                      const dt_remote_patch_t *patch,
                                      dt_remote_error_t **error);
gboolean dt_remote_vector_validate(const dt_remote_vector_descriptor_t *desc,
                                   const GArray *values /* double */,
                                   dt_remote_error_t **error);   // pure: count, ranges, ordering+gap
```

```c
/* remote_parameters.h — schema/value types, mirroring the curve ones */
typedef struct dt_remote_vector_component_schema_t
{ char *name; double minimum; double maximum; } dt_remote_vector_component_schema_t;

typedef struct dt_remote_vector_schema_t
{
  char *name; char *display_name; char *description;   // owned
  dt_remote_vector_subtype_t subtype;
  char *color_space;                                   // owned, nullable
  GArray *components;         // dt_remote_vector_component_schema_t (owned names)
  gboolean strictly_increasing;
  double minimum_gap;                                  // 0.0 = none
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;        // owned, nullable
  dt_remote_parameter_condition_t *writable_when;      // owned, nullable
} dt_remote_vector_schema_t;
void dt_remote_vector_schema_free(dt_remote_vector_schema_t *schema);

typedef struct dt_remote_vector_value_t
{
  char *name; GArray *values;  // double
  gboolean active, effective, writable_now;
} dt_remote_vector_value_t;
void dt_remote_vector_value_free(dt_remote_vector_value_t *value);
```

The Task 1 wrappers gain the `vector` union arms and
`dt_remote_semantic_schema_wrap_vector` / `dt_remote_semantic_value_wrap_vector`.

**Behavioral requirements** (each is a unit test in Step 1):

1. `registry_validate` fails closed on: native path not resolving to a
   `float` array leaf of length ≥ `component_count` + preserved tail
   (`native_capacity` must equal the resolved array length); duplicate IDs
   within the adapter; **an ID also registered by the curve registry for the
   same op** (cross-class uniqueness — call
   `dt_remote_curve_registry_lookup` and compare names); COLOR without
   `color_space`; LEVELS without `strictly_increasing`.
2. `list_schema` converts descriptors to owned schema structs in registry
   order; ops with no adapter yield `*out_fields = NULL`, return TRUE
   (silent degrade, curve precedent).
3. `read_values` resolves each descriptor against the live blob, widens
   floats to doubles, and stamps `active`/`writable_now` from the
   predicates against current params (copy the curve engine's predicate
   evaluation — `grep -n "writable_when" src/control/remote_curve.c`).
4. `apply_patch`: for each vector entry in `patch->semantic_values`
   (skip non-vector class ids): unknown ID → `DT_REMOTE_ERR_UNKNOWN_FIELD`;
   count ≠ `component_count` → `DT_REMOTE_ERR_INVALID_VALUE`;
   `dt_remote_vector_validate` in double domain (per-component min/max
   inclusive; LEVELS: `values[i+1] − values[i] >= minimum_gap`); writability
   predicate evaluated against `new_params` **after** scalar prepare-field
   writes are composed (mirror the curve engine's ordering exactly);
   narrowing writes only `[0..component_count)` floats; bytes
   `[component_count..native_capacity)` and every other blob byte
   unchanged; after all entries, `adapter->validate_completed` if non-NULL,
   with everything rolled back on failure (the caller applies to
   `temp_params`, curve precedent — remote_edit.c:1070).
5. `dt_remote_vector_validate` is pure and directly testable: reject
   count mismatch, component below min / above max (test at the double
   boundary: max 2.0, value 2.00000001 → reject — this is the reason for
   doubles), LEVELS unordered, LEVELS gap `< FLT_EPSILON`, accept gap
   `== FLT_EPSILON`.

- [x] **Step 1: Write failing unit tests** in `test_remote_vector.c` for
  requirements 1–5, using a **test-local static adapter** against a real
  loaded module `.so` exactly as `test_remote_curve.c` does (read its
  fixture setup first; reuse its module-loading fixture). Use `borders` as
  the fixture op for the generic tests (simple `color[3]` field) with a
  hand-rolled descriptor, so the engine is proven before any shipped
  adapter exists. Register the binary in
  `src/tests/unittests/control/CMakeLists.txt` by copying the
  `test_remote_curve` block.
- [x] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_vector`
  Expected: FAIL (nothing implemented).
- [x] **Step 3: Implement** `remote_vector.h/.c`,
  `remote_vector_registry.c` (empty `s_adapters[]` + lookup + validate),
  the schema/value types + frees in `remote_parameters.h/.c`, and the
  wrapper vector arms. Copy structure from the curve twins; do not invent
  new patterns. Add both `.c` files to the CMake source list.
- [x] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [x] **Step 5: Commit** — `remote_vector: vector-class engine and registry core`

### Task 4: Generalize the four remote_edit seams; wire serializers; capability

Spec §Protocol and engine changes item 4 and §Wire and MCP contract. After
this task, any registered vector adapter is fully live end-to-end.

**Files:**
- Modify: `src/control/remote_edit.c` — seams at `:892` (schema listing),
  `:823` (`_annotate_represented_by`), `:973` (readback), `:1070`/`:1085`
  (apply + verification)
- Modify: `src/control/remote_protocol.c` — serializers + hello `:583`
- Test: `src/tests/unittests/control/test_remote_edit.c`,
  `test_remote_protocol.c`

**Interfaces (Consumes):** everything Task 3 produced.

- [x] **Step 1: Write failing tests.**
  - `test_remote_edit.c`: with the vector registry's **test lookup
    override** (add `dt_remote_vector_registry_set_lookup_override`,
    mirroring remote_curve.h:239) injecting a borders test adapter:
    `get_module_schema("borders")` returns a `semantic_fields` entry tagged
    VECTOR alongside untouched primitive fields; `color`'s primitive field
    carries `represented_by == ["color"]`; params read returns the vector
    value; a full patch applies and reads back; readback-verification
    mismatch fails the mutation (curve tests show the harness pattern).
  - `test_remote_protocol.c`: schema JSON for the injected adapter contains
    `"class":"vector"`, `"subtype"`, `"components"` with per-component
    `name/minimum/maximum`, `"ordering"` only for LEVELS, `"color_space"`
    only for COLOR; value JSON carries `class/active/effective/
    writable_now/values`; hello capabilities contain `"vector_params"`.
- [x] **Step 2: Run to verify failure** (`ctest --test-dir build -R "remote_edit|remote_protocol"`).
- [x] **Step 3: Implement.**
  - **Schema seam (:892):** after `dt_remote_curve_list_schema`, call
    `dt_remote_vector_list_schema`; wrap both outputs (curves first, then
    vectors — deterministic aggregation order per spec) into the tagged
    container.
  - **Annotation seam (:823):** extract the per-path stamping loop
    (:844–877) into
    `static gboolean _stamp_root(dt_remote_module_schema_t *schema, const
    dt_remote_introspection_path_t *path, const char *semantic_name,
    dt_remote_error_t **error)`; curve descriptors stamp their three paths,
    vector descriptors stamp their single `native` path. Aliased arrays
    (colorbalance, rgblevels row 0) accumulate multiple names — the
    existing dedupe loop already handles this.
  - **Readback seam (:973) and apply seam (:1070/:1085):** two-call
    sequences; vector entries in one patch route to
    `dt_remote_vector_apply_patch`, curve entries to the curve engine, both
    against the same `temp_params`, single readback-verification pass over
    the merged `semantic_values` map.
  - **Serializers:** `_vector_schema_to_json` / `_vector_value_to_json`
    modeled member-for-member on `_curve_schema_to_json` (:435) /
    `_curve_value_to_json` (:492), emitting exactly the members the Step 1
    tests assert; the Task 1 `g_assert_not_reached()` arms become these
    calls. Reuse `_condition_to_json` for `writable_when`/`active_when`.
  - **Hello:** add `"vector_params"` immediately after the
    `"curve_params"` line (remote_protocol.c:583).
- [x] **Step 4: Run all suites** — `ctest --test-dir build` PASS; curve
  suites prove byte-identical curve behavior.
- [x] **Step 5: Commit** — `remote_edit: dispatch semantic seams across curve and vector registries`

### Task 5: colorbalance adapter — six mode-gated aliases

Spec §Module adapter specifics, colorbalance. Storage: `lift[4]`,
`gamma[4]`, `gain[4]`, order `factor,red,green,blue`, all 0.0–2.0
(colorbalance.c:100); `mode` enum `LIFT_GAMMA_GAIN=0, SLOPE_OFFSET_POWER=1
(default), LEGACY=2` (colorbalance.c:58).

**Files:**
- Modify: `src/control/remote_vector_registry.c`
- Test: `src/tests/unittests/control/test_remote_vector.c`

**Descriptor data (normative):**

```c
static const dt_remote_vector_component_t s_colorbalance_components[4] =
  { { "factor", 0.0, 2.0 }, { "red", 0.0, 2.0 },
    { "green", 0.0, 2.0 },  { "blue", 0.0, 2.0 } };

static const dt_remote_path_segment_t S_CB_LIFT[]  = {{ DT_REMOTE_PATH_FIELD, { .field = "lift"  } }};
static const dt_remote_path_segment_t S_CB_GAMMA[] = {{ DT_REMOTE_PATH_FIELD, { .field = "gamma" } }};
static const dt_remote_path_segment_t S_CB_GAIN[]  = {{ DT_REMOTE_PATH_FIELD, { .field = "gain"  } }};
/* predicates: field "mode", enum names "SLOPE_OFFSET_POWER";
   lift/gamma/gain → NE, offset/power/slope → EQ.
   Declare them exactly as colorzones' channel predicates are declared in
   remote_curve_registry.c (same dt_remote_parameter_predicate_t type). */
```

Six descriptors over three paths — alias pairs (lift,offset)→`lift`,
(gamma,power)→`gamma`, (gain,slope)→`gain`; all `component_count 4`,
`native_capacity 4`, subtype PLAIN. `prepare_fields = { "mode" }`.
Descriptions are required to state (spec, verbatim requirement): stored 1.0
is the identity — the GUI displays it as 0.0 (red/green/blue) or 0%
(factor) — and the color space (ProPhoto RGB; LEGACY mode is sRGB).

- [x] **Step 1: Write failing tests** (real `colorbalance` .so, version 3):
  - schema lists exactly `lift,gamma,gain,offset,power,slope` in that
    order, each with the four components and `writable_when` on `mode`;
  - component-order proof **against an independently constructed blob**:
    build a params blob via introspection defaults, poke
    `lift = {1.0, 1.25, 1.0, 1.0}` directly at the introspection-resolved
    offset, read values, assert component 1 ("red") == 1.25 — not readback
    of our own write;
  - gating: default params (mode SOP) → `offset` writable_now TRUE, `lift`
    FALSE; patch writing `lift` under SOP → `DT_REMOTE_ERR_INVALID_VALUE`
    (not-writable error shape — match what curves return for a gated-off
    write, see the colorzones tests);
  - composed mode switch: one patch with scalar `mode = LIFT_GAMMA_GAIN` +
    vector `lift` succeeds; blob's `gamma`/`gain` bytes untouched;
  - alias write-conflict is structurally impossible: patch with both
    `lift` and `offset` → exactly one is non-writable under any mode →
    rejected atomically; assert blob unchanged.
- [x] **Step 2: Run to verify failure** (`ctest --test-dir build -R remote_vector`).
- [x] **Step 3: Implement** the descriptor table + adapter row
  `{ "colorbalance", 3, 3, s_colorbalance_vectors, 6, s_colorbalance_prepare, 1, NULL }`.
- [x] **Step 4: Run tests** — PASS; full `ctest` PASS.
- [x] **Step 5: Commit** — `remote_vector_registry: colorbalance mode-gated lift/gamma/gain + offset/power/slope`

### Task 6: channelmixerrgb adapter — six rows + normalize zero-sum guard

Spec §Module adapter specifics, channelmixerrgb. Rows `red/green/blue`
(−2.0…2.0, channelmixerrgb.c:94–96) and `saturation/lightness/grey`
(−2.0…2.0, :97–99), all `float[4]`; fourth component reserved (resolved
decision above). Guard: reject normalize-enabled row summing to exactly
`0.0f` after float narrowing (spec's exact comparison, matching the
module's own `norm_grey == 0.f` guard).

**Files:**
- Modify: `src/control/remote_vector_registry.c`
- Test: `src/tests/unittests/control/test_remote_vector.c`

**Descriptor data (normative):** six descriptors, subtype PLAIN,
`component_count 3`, `native_capacity 4`, components
`{ "red", −2.0, 2.0 }, { "green", −2.0, 2.0 }, { "blue", −2.0, 2.0 }`
(input-channel contributions; identical triple for all six rows), paths
`FIELD "red"` … `FIELD "grey"`. No predicates. `prepare_fields` lists the
three normalize flags so the guard sees composed scalar writes:
`{ "normalize_R", "normalize_G", "normalize_B" }` (verify exact field names
with `grep -n "normalize_" src/iop/channelmixerrgb.c | head`).

**Guard (normative shape — resolve fields via paths, never structs):**

```c
static gboolean _cmrgb_guard_row(const dt_remote_vector_context_t *ctx, void *params,
                                 const char *row_field, const char *flag_field,
                                 dt_remote_error_t **error)
{
  const dt_remote_path_segment_t row_seg[]  = {{ DT_REMOTE_PATH_FIELD, { .field = row_field  } }};
  const dt_remote_path_segment_t flag_seg[] = {{ DT_REMOTE_PATH_FIELD, { .field = flag_field } }};
  const dt_remote_introspection_path_t row_path  = { row_seg, 1 };
  const dt_remote_introspection_path_t flag_path = { flag_seg, 1 };
  void *row_ptr = NULL, *flag_ptr = NULL;
  if(!dt_remote_path_resolve(&row_path, ctx->introspection->field, params, NULL, &row_ptr, error)
     || !dt_remote_path_resolve(&flag_path, ctx->introspection->field, params, NULL, &flag_ptr, error))
    return FALSE;   // registry/introspection drift, internal error
  const float *row = row_ptr;
  const gboolean *flag = flag_ptr;
  const float sum = row[0] + row[1] + row[2];
  if(*flag && sum == 0.0f)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE,
      _("channelmixerrgb '%s' sums to zero while '%s' is enabled; "
        "this would produce a non-finite render"), row_field, flag_field);
    return FALSE;
  }
  return TRUE;
}
static gboolean _cmrgb_validate_completed(const dt_remote_vector_context_t *ctx,
                                          const void *new_params, dt_remote_error_t **error)
{ /* three _cmrgb_guard_row calls: red/normalize_R, green/normalize_G, blue/normalize_B */ }
```

- [x] **Step 1: Write failing tests** (real .so, version 3):
  - schema: six rows, three components each, ranges ±2.0;
  - reserved-component preservation: seed `red[3] = 0.777f` directly in the
    blob, patch `red = [1,0,0]`, assert `red[3]` still `0.777f` and
    `red[0..2]` written;
  - guard rejects: scalar `normalize_R = TRUE` (introspection default is
    off — verify) composed with vector `red = [1,-1,0]` →
    `DT_REMOTE_ERR_INVALID_VALUE`, blob unchanged (atomic rollback);
  - guard permits: `normalize_R = TRUE` + `red = [1,-1,0.5]` (near-zero
    but not exactly zero) succeeds — parity bound from the spec;
  - guard covers flag-only writes: rows already `[1,-1,0]` in old params,
    patch flips only `normalize_R` → rejected;
  - independent-read proof for one row (same poke-the-blob pattern as
    Task 5).
- [x] **Step 2: Run to verify failure.**
- [x] **Step 3: Implement** descriptors + guard + adapter row
  `{ "channelmixerrgb", 3, 3, s_cmrgb_vectors, 6, s_cmrgb_prepare, 3, _cmrgb_validate_completed }`.
- [x] **Step 4: Run tests** — PASS; full `ctest` PASS.
- [x] **Step 5: Commit** — `remote_vector_registry: channelmixerrgb mixing rows with normalize zero-sum guard`

### Task 7: rgblevels adapter — four gated names, no reset, epsilon gaps

Spec §Module adapter specifics, rgblevels. `levels[3][3]` 0.0–1.0,
`autoscale` enum default `DT_IOP_RGBLEVELS_LINKED_CHANNELS`
(rgblevels.c:54); version 1.

**Files:**
- Modify: `src/control/remote_vector_registry.c`
- Test: `src/tests/unittests/control/test_remote_vector.c`

**Descriptor data (normative):** subtype LEVELS, components
`{ "black", 0.0, 1.0 }, { "grey", 0.0, 1.0 }, { "white", 0.0, 1.0 }`,
`strictly_increasing TRUE`, `minimum_gap FLT_EPSILON`,
`component_count 3 == native_capacity`. Paths use a row index segment:

```c
static const dt_remote_path_segment_t S_RGBLEVELS_ROW0[] =
  {{ DT_REMOTE_PATH_FIELD, { .field = "levels" } }, { DT_REMOTE_PATH_INDEX, { .index = 0 } }};
/* rows 1 and 2 likewise */
```

Four descriptors: `levels.linked` (row 0, writable_when `autoscale EQ
DT_IOP_RGBLEVELS_LINKED_CHANNELS`), `levels.red`/`levels.green`/
`levels.blue` (rows 0/1/2, writable_when `EQ
DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS`). `prepare_fields = { "autoscale" }`.
**No validate_completed, no reset of any row, ever** — the module itself
preserves stored rows across autoscale switches (rgblevels.c:747
`gui_changed` only flips the displayed tab; the row-0 fan-out is
pipeline-only, :857).

- [x] **Step 1: Write failing tests** (real .so, version 1):
  - schema: four names, LEVELS ordering advertised
    (`minimum_gap == FLT_EPSILON`, at-least);
  - default (linked): `levels.linked` writable_now TRUE, the three
    channel names FALSE; write `levels.linked = [0.1, 0.5, 0.9]` → row 0
    written, **rows 1 and 2 byte-identical** to before;
  - composed switch: patch `autoscale = INDEPENDENT_CHANNELS` +
    `levels.green = [0.2, 0.4, 0.8]` succeeds; row 0 and row 2 untouched
    (no-reset proof);
  - switch back: patch only `autoscale = LINKED_CHANNELS` (scalar-only)
    succeeds and **no row changes** — the no-reset guarantee for
    mode-only writes;
  - ordering: `[0.5, 0.5, 0.9]` rejected; `[0.5, 0.4, 0.9]` rejected;
    gap `== FLT_EPSILON` accepted (`{0.5, 0.5f + FLT_EPSILON, 0.9}` widened
    to double), gap below it rejected;
  - aliasing: linked write then switch to independent and read
    `levels.red` — returns the values written via `levels.linked`
    (same storage row).
- [x] **Step 2: Run to verify failure.**
- [x] **Step 3: Implement** the four descriptors + adapter row
  `{ "rgblevels", 1, 1, s_rgblevels_vectors, 4, s_rgblevels_prepare, 1, NULL }`.
- [x] **Step 4: Run tests** — PASS; full `ctest` PASS.
- [x] **Step 5: Commit** — `remote_vector_registry: rgblevels gated levels triples, epsilon minimum gap, no reset`

### Task 8: borders and watermark adapters — color subtype

Spec §Module adapter specifics, borders/watermark. borders v4:
`color[3]` ($DEFAULT 1.0, borders.c:82), `frame_color[3]` ($DEFAULT 0.0,
:103). watermark v7: `color[3]` ($DEFAULT 0.0, watermark.c:109). No
`$MIN/$MAX` on any — descriptor ranges 0.0–1.0 are normative (resolved
decision).

**Files:**
- Modify: `src/control/remote_vector_registry.c`
- Test: `src/tests/unittests/control/test_remote_vector.c`

**Descriptor data (normative):** subtype COLOR, `color_space
"display_rgb"`, components `{ "red", 0.0, 1.0 }, { "green", 0.0, 1.0 },
{ "blue", 0.0, 1.0 }`, `component_count 3 == native_capacity`, no
predicates, no prepare fields, no validate_completed. borders: two
descriptors `color` (path `FIELD "color"`) and `frame_color`
(`FIELD "frame_color"`); adapter row `{ "borders", 4, 4, ..., 2, NULL, 0,
NULL }`. watermark: one descriptor `color`; row `{ "watermark", 7, 7, ...,
1, NULL, 0, NULL }`.

- [x] **Step 1: Write failing tests** (real .so's, versions 4 and 7):
  schema advertises subtype `color` + `color_space "display_rgb"`;
  independent-read pokes; round-trip writes; out-of-range `1.00000001`
  rejected in double domain; borders `frame_color` write leaves `color`
  and every scalar byte untouched.
- [x] **Step 2: Run to verify failure.**
- [x] **Step 3: Implement** the three descriptors + two adapter rows.
- [x] **Step 4: Run tests** — PASS; full `ctest` PASS. All five adapters
  now registered: 19 semantic names total (6+6+4+2+1 — assert this count
  in a registry test).
- [x] **Step 5: Commit** — `remote_vector_registry: borders and watermark color triples`

### Task 9: Python sidecar — `vectors` argument

Spec §Wire and MCP contract, MCP tool surface. Mirrors the `curves`
plumbing at tools/mcp/src/darktable_mcp/server.py:74 (`_wire_semantic_values`)
and :176/:210 (tool arg + `curve_params` gate).

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py` (tool behavior),
  `tools/mcp/tests/test_protocol.py` (if translation helpers are tested
  there — follow where `_wire_semantic_values` tests live:
  `grep -rn "_wire_semantic_values" tools/mcp/tests/`)

- [x] **Step 1: Write failing tests** (fake-server pattern the file already
  uses):
  - `vectors={"lift": [1.0, 1.1, 1.0, 0.95]}` sends
    `semantic_values={"lift": {"class": "vector", "values": [1.0, 1.1, 1.0, 0.95]}}`;
  - `curves` and `vectors` in one call merge into one `semantic_values`
    dict; a semantic ID appearing in both raises `ToolError` before any
    wire traffic;
  - `vectors` against a server not advertising `vector_params` raises the
    upgrade `TransportError` (mirror the `curve_params` wording); `curves`
    alone still requires only `curve_params`;
  - rejects: empty list, non-list, bool element, NaN/inf element,
    non-numeric element — each `ToolError` with the offending name, no
    connection attempted.
- [x] **Step 2: Run to verify failure:** `cd tools/mcp && .venv/bin/pytest -q`
  Expected: FAIL (no `vectors` parameter).
- [x] **Step 3: Implement.**

```python
def _wire_vector_values(vectors: dict[str, Any]) -> dict[str, Any]:
    """Translate the `vectors` tool argument to wire semantic_values
    entries. Mirrors _wire_semantic_values: validate fully before any
    wire traffic."""
    out: dict[str, Any] = {}
    for name, values in vectors.items():
        ok = (isinstance(values, (list, tuple)) and len(values) > 0
              and all(isinstance(v, (int, float)) and not isinstance(v, bool)
                      and math.isfinite(v) for v in values))
        if not ok:
            raise ToolError(
                f"vector '{name}' must be a non-empty list of finite numbers")
        out[name] = {"class": "vector", "values": [float(v) for v in values]}
    return out
```

  In `set_module_params`: add `vectors: dict[str, Any] | None = None`;
  translate both arguments up front; raise `ToolError` on key overlap;
  gate each on its own capability (`curve_params` / `vector_params`,
  identical wording pattern to server.py:213–217); merge into
  `params["semantic_values"]`. Extend the docstring: semantic vector IDs
  come from `get_module_schema` `semantic_fields` with `"class":
  "vector"`; each patch replaces the whole named vector; components are
  stored-space values (colorbalance stores identity as 1.0); requires
  `vector_params`.
- [x] **Step 4: Run tests** — `cd tools/mcp && .venv/bin/pytest -q` PASS.
- [x] **Step 5: Commit** — `mcp sidecar: vectors argument on set_module_params, vector_params gated`

### Task 10: Live integration gates

Spec §Testing item 3. New file patterned on
`tools/mcp/tests/integration/test_curves_milestone3.py` (read it first —
session fixtures, render-diff helpers, history assertions all exist there).

**Files:**
- Create: `tools/mcp/tests/integration/test_vectors_milestone4.py`
- Modify: `tools/mcp/tests/integration/harness.py` — only if launch-time
  sidecar drop needs a hook: add optional `sidecar_files: dict[str, bytes]`
  written into `self.workdir` next to the copied image in `launch()`
  (harness.py:326), default empty.

**Gates (each is one test; write them all before running any):**

1. **colorbalance:** default mode (SOP) — write `offset/power/slope`,
   read back, render-diff vs baseline; one patch `mode=LIFT_GAMMA_GAIN` +
   `lift` write succeeds; `lift` write without the mode switch fails with
   `invalid_value` and history length unchanged.
2. **channelmixerrgb:** write `red=[0.8, 0.1, 0.1]`, readback + render-diff;
   guard gate: `values={"normalize_R": True}` + `vectors={"red": [1,-1,0]}`
   → error, `get_module_params` proves nothing changed.
3. **rgblevels:** `values={"autoscale": "DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS"}`
   + `vectors={"levels.red": [0.05, 0.4, 0.95]}` in one call; readback of
   all four names shows green/blue rows at defaults (no reset); render-diff;
   ordering rejection `[0.5, 0.5, 0.9]` → `invalid_value`.
4. **borders:** enable + `values={"frame_size": 0.1}` +
   `vectors={"frame_color": [1.0, 0.0, 0.0]}` — frame_size makes the color
   effective (spec fixture requirement); render-diff; second gate for
   `color`.
5. **watermark:** XMP-seeded fixture (below), enable watermark,
   `vectors={"color": [1.0, 0.0, 0.0]}`, render-diff proves effectiveness
   (`simple-text.svg` consumes `$(WATERMARK_COLOR)`; default
   `darktable.svg` does not — watermark.c:1387).
6. **undo:** vector write → `undo` → readback equals pre-write values
   (mirrors test_curves.py:178).
7. **stale revision:** vector patch with stale `expected_revision` changes
   nothing (mirrors test_curves.py:307).
8. **curve regression:** run the existing curve integration files in the
   same session run — no code, just include them in the invocation.

**Watermark XMP fixture helper** (module-level in the new test file):

```python
def _watermark_params_v7(filename: str, text: str = "MCP") -> bytes:
    """dt_iop_watermark_params_t, version 7 (src/iop/watermark.c:88-112):
    float opacity, scale, xoffset, yoffset; int alignment; float rotate;
    int scale_base, scale_img, scale_svg; char filename[256];
    char text[512]; float color[3]; char font[64]."""
    return struct.pack(
        "<4f i f 3i 256s 512s 3f 64s",
        100.0, 100.0, 0.0, 0.0, 4, 0.0, 0, 0, 0,
        filename.encode(), text.encode(), 0.0, 0.0, 0.0,
        b"DejaVu Sans 10")
```

During implementation, capture the XMP history-entry template once from a
live run (enable watermark via MCP, copy the written `.xmp`, replace the
`darktable:params` hex with `_watermark_params_v7(...).hex()` and pin
`darktable:modversion="7"`), commit the template string into the test.
Verify `struct.calcsize` == the C `sizeof` (assert 880 == 4*4+4+4+3*4+256+512+12+64
in a unit-style check inside the test module).

- [x] **Step 1: Write all gates.**
- [x] **Step 2: Run.**
  `cd tools/mcp && DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`
  Expected: new gates PASS, every existing curve gate PASS.
- [x] **Step 3: Commit** — `mcp tests: milestone 4 live vector gates for the five adapters`

### Task 11: Documentation

Spec §Documentation — same milestone, not after.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `tools/mcp/README.md`, `tools/mcp/docs/remote-control.md`
- Modify: `docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-vector-class-design.md` (status line → implemented)

- [x] **Step 1: supported-operations.** Move `colorbalance`,
  `channelmixerrgb`, `borders` rows (:114–116) from Tier 2 to Tier 1;
  move `rgblevels` (:151) from Tier 3 to Tier 1 and drop it from the
  Tier-3 rationale sentence (:138); update `watermark` (:130) to note the
  color vector is supported, strings still not (stays Tier 2 / near-Tier 1
  wording per spec). Update the tier counts in the section headers
  (:59, :110, :132). Appendix: add
  `| borders | aspect_text, pos_h_text, pos_v_text | marked UNUSED in source |`.
  Note in each moved row that arrays are exposed as semantic vectors with
  `represented_by`, matching the curve rows' phrasing.
- [x] **Step 2: protocol-reference.** In the capability section (:76):
  `vector_params` advertises writable vector-class semantic parameters,
  never advertised without accepting vector entries. Add the normative
  schema/value/patch JSON shapes and the stable vocabulary (subtypes,
  `display_rgb`, `at_least`) — copy them from spec §Wire and MCP contract
  verbatim.
- [x] **Step 3: MCP user docs.** README + remote-control.md: `vectors`
  argument with a colorbalance SOP example and the stored-identity warning
  (1.0 = identity, GUI shows 0.0/0%); rgblevels linked/independent
  example.
- [x] **Step 4: Verify all suites one final time** (full ctest + pytest +
  integration) and commit —
  `darktable-mcp: milestone 4 docs — tiers, vector protocol, sidecar usage`

---

## Self-review notes (already applied)

- Spec coverage: every spec section maps to a task — class model/enum
  (T2), descriptor paths (T3), containers/serializers (T1/T4), parser
  doubles+caps (T2), wire contract + capability + tool arg (T4/T9),
  colorbalance aliases (T5), cmrgb guard (T6), rgblevels no-reset+gaps
  (T7), borders/watermark + appendix (T8/T11), per-descriptor tests
  (T5–T8), effective fixtures + undo/stale gates (T10), docs (T11).
- Type consistency: `dt_remote_vector_*` names are used identically across
  Tasks 2–9; the wrapper helpers from Task 1 gain vector arms in Task 3.
- The 19-name count (spec §Testing) is asserted in Task 8 Step 4.
