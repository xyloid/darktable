# darktable MCP Milestone 6 — Quantity Class (White Balance) + Vector Mop-Up — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the quantity semantic class (wire name `"quantity"`)
with its first adapter — `temperature` Kelvin/tint white balance — plus
the negadoctor and colorharmonizer vector ride-along adapters and the
sidecar `quantities` argument, per the approved design
`docs/superpowers/specs/2026-07-17-darktable-mcp-milestone6-quantity-class-design.md`
(as amended: the sidecar DOES gain a `quantities` sugar argument). That
spec is normative; where this plan says "per spec §X" the implementer
must read that section.

**Architecture:** Milestones 2–5 built three class engines (curve,
vector, bands) behind a class-ops dispatch table in `remote_edit.c`, each
reading/writing params blobs via introspection offsets. Milestone 6 adds
a fourth engine whose defining difference is a **module-owned
conversion**: a new optional iop API function pair (declared in
`src/iop/iop_api.h`, resolved by symbol name at plugin load, implemented
only by `temperature.c`) converts between stored coefficients and the
Kelvin/tint presentation domain. The engine validates in the presentation
domain, calls the hook to write, and byte-checks that only the adapter's
declared native fields changed. The ride-along adapters are pure static
data on the untouched m4 vector engine.

**Tech Stack:** C (GLib, JSON-GLib, darktable introspection, GModule
plugin symbols), cmocka unit tests under `src/tests/unittests/control/`,
Python 3 + FastMCP sidecar in `tools/mcp/`, pytest integration harness
under Xvfb.

## Global Constraints

- Work in the git worktree `<REPO>/.claude/worktrees/mcp-remote-edit`, branch `worktree-mcp-remote-edit`. All paths below are relative to that root.
- TDD: every behavior change lands with its failing test first.
- Commit messages end with the two trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_017UPe8bYYZLQ8aasRge4kBP`.
- Verification at every commit: `cmake --build build -j$(nproc)` then
  `ctest --test-dir build` (all suites), `cd tools/mcp && .venv/bin/pytest -q`;
  for Task 8 also `DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`.
- **Curve, vector, AND bands behavior byte-for-byte unchanged.** The
  existing `test_remote_curve`, `test_remote_vector`, `test_remote_band`,
  protocol/edit suites, and the sidecar suites are the regression net.
- Params versions (verified from `DT_MODULE_INTROSPECTION`):
  `temperature` = 4, `negadoctor` = 2, `colorharmonizer` = 1. Every
  adapter pins `minimum_params_version == maximum_params_version` to
  exactly these.
- Stable wire vocabulary (spec §Wire and MCP contract): class
  `"quantity"`; capability `"quantity_params"`; schema members
  `components` (each `{name, unit?, minimum, maximum}`), `derived`;
  value/patch member `values` (object keyed by component name); error
  details `{"parameter", "component", "constraint"}` (`component` is the
  offending component's **name**, present only when attributable).
- Quantity component domains (verified constants, `temperature.c:50-54`):
  `temperature` unit `kelvin` [1901.0, 25000.0]
  (`DT_IOP_LOWEST/HIGHEST_TEMPERATURE`); `tint` no unit [0.135, 2.326]
  (`DT_IOP_LOWEST/HIGHEST_TINT`).
- Component values travel as **doubles** end-to-end; the write hook
  narrows to native `float` storage.
- Wire patch entries require `"class": "quantity"` — the parser's
  "class is required in mutation values" rule applies to all classes.
- **Coefficient coexistence (spec §Coefficient coexistence)**:
  `temperature`'s `red`/`green`/`blue`/`various` scalars STAY writable
  and additionally carry `represented_by: ["wb.temperature"]`. A request
  mixing any of those scalar writes with a `wb.temperature` entry is
  rejected `invalid_value`, constraint `"native_conflict"`. Do NOT
  demote the coeffs to `writable: false` — this is a documented
  deliberate exception to the natives-go-read-only precedent.
- **The conversion needs live GUI state**: `_mul2temp`/`_temp2mul` read
  `self->gui_data->CAM_to_XYZ` (`temperature.c:475,489`). The
  temperature hooks return FALSE when `self->gui_data == NULL` (fail
  closed), and true conversion round-trips are proven in the integration
  gates only — the unit harness (`dt_iop_load_module` without GUI init,
  see `test_remote_band.c:687-690`) cannot run them. Unit tests use the
  fake-hook seam instead.

## Decisions resolved during planning (from spec §Open items)

- **iop API names/signatures** (mirrors `iop_api.h`'s `OPTIONAL(...)`
  rows, e.g. line 127):

```c
/* src/iop/iop_api.h — component order is the registry descriptor order;
   count is the component count and both hooks must reject a mismatch. */
OPTIONAL(gboolean, remote_quantity_read, struct dt_iop_module_t *self,
         const dt_iop_params_t *params, double *values, size_t count);
OPTIONAL(gboolean, remote_quantity_write, struct dt_iop_module_t *self,
         const double *values, size_t count, dt_iop_params_t *params);
```

- **Vector padded-length**: already supported — `component_count` 3 with
  `native_capacity` 4 preserves trailing storage
  (`remote_vector.h:66-69`). No engine change for negadoctor.
- **Semantic names** (native-field-name convention from m4): negadoctor
  `dmin`, `wb_high`, `wb_low`; colorharmonizer `custom_hue`,
  `node_saturation`; temperature `wb.temperature`.
- **Kelvin readback tolerance**: integration gates assert
  `abs(read_kelvin - written_kelvin) <= 5.0` and
  `abs(read_tint - written_tint) <= 0.01`; Task 8 first prints the
  measured deltas so the pin is grounded in observed quantization.
- **display_name sourcing**: static `const char *` on the descriptor,
  untranslated — exactly what the curve/vector/band registries do.
- **Sidecar**: per the amended spec, a `quantities` argument IS added
  (Task 7); the original no-sugar draft was wrong because the MCP tool
  has no raw `semantic_values` passthrough.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/iop/iop_api.h` | modify | Task 2: the two `OPTIONAL` hook declarations |
| `src/iop/temperature.c` | modify | Task 2: `remote_quantity_read`/`remote_quantity_write` wrappers over `_mul2temp`/`_temp2mul` |
| `src/control/remote_parameters.h/.c` | modify | Task 1: `DT_REMOTE_PARAMETER_QUANTITY`, quantity patch/schema/value types + frees, wrapper `quantity` union arms |
| `src/control/remote_protocol.c` | modify | Task 1: parser branch for class `"quantity"`; Task 4: `_quantity_schema_to_json`/`_quantity_value_to_json`, `quantity_params` in hello |
| `src/control/remote_quantity.h/.c` | create | Task 3: engine — descriptor/adapter types, `list_schema`, `read_values`, `apply_entries`, pure validator, hook dispatch + byte-check, native-conflict rule |
| `src/control/remote_quantity_registry.c` | create | Task 3: registry infrastructure + test seams (empty adapter table); Task 5: temperature adapter |
| `src/control/remote_edit.c` | modify | Task 4: quantity row in `s_class_ops[]` (`remote_edit.c:1036`) + `_annotate_quantity_represented_by` |
| `src/control/remote_vector_registry.c` | modify | Task 6: negadoctor + colorharmonizer adapters |
| `src/tests/unittests/control/test_remote_quantity.c` | create | Tasks 3/5 unit tests |
| `src/tests/unittests/control/test_remote_protocol.c` | modify | Tasks 1/4 parser + serializer + capability tests |
| `src/tests/unittests/control/test_remote_edit.c` | modify | Task 4 dispatch/annotation/conflict tests |
| `src/tests/unittests/control/test_remote_vector.c` | modify | Task 6 per-adapter tests |
| `src/tests/unittests/control/CMakeLists.txt` | modify | Task 3: register `test_remote_quantity` |
| `src/tests/unittests/control/fixtures/hello_response.json` | modify | Task 4: add `"quantity_params"` |
| `tools/mcp/src/darktable_mcp/server.py` | modify | Task 7: `_wire_quantity_values`, `quantities` argument, four-way overlap, gate |
| `tools/mcp/tests/test_tools.py`, `tests/test_protocol.py` | modify | Task 7 sidecar tests |
| `tools/mcp/tests/integration/test_quantity_milestone6.py` | create | Task 8 live gates |
| docs (see Task 9) | modify | tiers, milestone row, protocol reference, README/remote-control, design status |

The CMake source list that builds `remote_band_registry.c` (find it with
`grep -rn "remote_band_registry" src/CMakeLists.txt`) gains
`remote_quantity.c` and `remote_quantity_registry.c` in Task 3.

---

### Task 1: Quantity class core — enum, neutral types, parser branch

Spec §Wire and MCP contract (patch entry). Mirrors m5 Task 2 exactly.

**Files:**
- Modify: `src/control/remote_parameters.h/.c`
- Modify: `src/control/remote_protocol.c` (the parser class dispatch —
  find it: `grep -n '"bands"' src/control/remote_protocol.c`)
- Test: `src/tests/unittests/control/test_remote_protocol.c`

**Interfaces (Produces):**

```c
/* remote_parameters.h */
DT_REMOTE_PARAMETER_QUANTITY   /* appended to dt_remote_parameter_class_t */

typedef struct dt_remote_quantity_component_value_t
{
  char *name;      // owned component name
  double value;
} dt_remote_quantity_component_value_t;

typedef struct dt_remote_quantity_patch_t
{
  char *name;        // owned semantic ID
  GPtrArray *values; // dt_remote_quantity_component_value_t*, owned, wire order
} dt_remote_quantity_patch_t;
/* added to dt_remote_semantic_patch_t's union as `quantity` */

/* remote_protocol.c */
#define DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP 8
```

- [ ] **Step 1: Write the failing parser tests** in
  `test_remote_protocol.c`, copying the bands parser test style (same
  fixtures/request helpers). Cases, each rejecting with
  `DT_REMOTE_ERR_INVALID_VALUE` and no patch, except the accepts:

```c
// accept: {"class":"quantity","values":{"temperature":5500.0,"tint":1.0}}
//   → patch with class_id DT_REMOTE_PARAMETER_QUANTITY, two component
//   values in wire order, doubles preserved
static void test_semantic_quantity_entry_parses(void **state);
// reject: values member missing / not an object / empty object
static void test_semantic_quantity_requires_values_object(void **state);
// reject: a component value that is non-numeric, boolean, NaN/Infinity, null
static void test_semantic_quantity_rejects_non_finite_components(void **state);
// reject: values object with 9 members (> DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP)
static void test_semantic_quantity_rejects_oversized_component_object(void **state);
// reject: unknown entry member {"class":"quantity","values":{...},"points":[...]}
static void test_semantic_quantity_rejects_unknown_members(void **state);
// double-domain proof: 5500.00000001 survives as double
static void test_semantic_quantity_preserves_double_precision(void **state);
```

- [ ] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_protocol`
  Expected: FAIL (`"unsupported class 'quantity'"` path today).
- [ ] **Step 3: Implement.** Append the enum member; union arm
  `dt_remote_quantity_patch_t quantity;` with a free-function case
  (frees `name`, unrefs `values` whose element free func frees the
  component name and struct). Parser: add the `"quantity"` branch beside
  `"bands"` — allowed members `class`, `values` only; `values` a
  required JSON object with 1..8 members, every member value through
  `_node_to_finite_double`, member order preserved into the GPtrArray.
  Duplicate-ID detection across classes already runs before the class
  switch — verify with the existing cross-class test pattern, don't
  duplicate the mechanism.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build -R remote_protocol` — PASS; then full
  `ctest --test-dir build` — PASS.
- [ ] **Step 5: Commit** — `remote_protocol: parse quantity-class semantic values (named components, doubles)`

### Task 2: Optional iop API hook pair + temperature implementation

Spec §Conversion authority. The milestone's one new mechanism.

**Files:**
- Modify: `src/iop/iop_api.h` — the two `OPTIONAL` rows (place them
  after the `color_picker_apply` block, `iop_api.h:133`, with a comment
  block in the file's house style)
- Modify: `src/iop/temperature.c` — the two exported functions
- Test: `src/tests/unittests/control/test_remote_quantity.c` does the
  presence checks in Task 3; this task's own verification is
  compile-level plus a temporary assertion (Step 3)

**Interfaces (Produces):** the two `OPTIONAL` declarations from
"Decisions resolved during planning" above, and in `temperature.c`:

```c
// Remote-edit quantity hooks (darktable MCP): convert between the stored
// channel coefficients and the Kelvin/tint presentation pair. Component
// order is fixed by the remote quantity registry: [0] temperature (K),
// [1] tint. Both need the GUI's camera matrices (gui_data->CAM_to_XYZ /
// XYZ_to_CAM), so they fail closed without a built GUI.
gboolean remote_quantity_read(dt_iop_module_t *self,
                              const dt_iop_params_t *params,
                              double *values, size_t count)
{
  if(!self || !self->gui_data || !params || !values || count != 2)
    return FALSE;
  dt_iop_temperature_params_t p = *(const dt_iop_temperature_params_t *)params;
  float TempK = 0.0f, tint = 0.0f;
  _mul2temp(self, &p, &TempK, &tint);
  values[0] = TempK;
  values[1] = tint;
  return TRUE;
}

gboolean remote_quantity_write(dt_iop_module_t *self,
                               const double *values, size_t count,
                               dt_iop_params_t *params)
{
  if(!self || !self->gui_data || !params || !values || count != 2)
    return FALSE;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)params;
  double mul[4] = { 0.0 };
  _temp2mul(self, values[0], values[1], mul);
  p->red = mul[0];
  p->green = mul[1];
  p->blue = mul[2];
  p->various = mul[3];
  return TRUE;
}
```

- [ ] **Step 1: Add the two `OPTIONAL` declarations** to `iop_api.h`
  exactly as specified, matching the neighboring rows' formatting.
- [ ] **Step 2: Implement the two functions in `temperature.c`** as
  above (place them directly after `_mul2temp`, `temperature.c:496`).
  They must NOT be `static` — plugin symbol resolution finds them by
  name.
- [ ] **Step 3: Build and spot-check symbol resolution.**
  Run: `cmake --build build -j$(nproc)`
  Expected: clean build (the OPTIONAL machinery makes absent
  implementations legal for every other module).
  Then: `nm -D build/lib/darktable/plugins/libtemperature.so | grep remote_quantity`
  Expected: both symbols exported. Also check one other module:
  `nm -D build/lib/darktable/plugins/libexposure.so | grep remote_quantity`
  Expected: no output.
- [ ] **Step 4: Run all suites** (`ctest --test-dir build`) — PASS,
  zero test edits (pure addition).
- [ ] **Step 5: Commit** — `iop api: optional remote_quantity read/write hooks; temperature implements Kelvin/tint`

### Task 3: Quantity engine and registry core

Spec §Class model, §Conversion authority, §Coefficient coexistence.
Mirrors the band split: `remote_quantity.c` = engine,
`remote_quantity_registry.c` = static data + seams.

**Files:**
- Create: `src/control/remote_quantity.h`, `src/control/remote_quantity.c`
- Create: `src/control/remote_quantity_registry.c` (infrastructure +
  empty adapter table; Task 5 fills it)
- Modify: the CMake source list that builds `remote_band_registry.c`
- Create: `src/tests/unittests/control/test_remote_quantity.c`
- Modify: `src/tests/unittests/control/CMakeLists.txt` (copy the
  `test_remote_band` block)

**Interfaces (Produces):**

```c
/* remote_quantity.h */
typedef struct dt_remote_quantity_component_descriptor_t
{
  const char *name;
  const char *unit;        // NULL = unitless
  double minimum, maximum;
} dt_remote_quantity_component_descriptor_t;

typedef struct dt_remote_quantity_descriptor_t
{
  const char *name;               // stable semantic ID, e.g. "wb.temperature"
  const char *display_name;
  const char *description;
  const dt_remote_quantity_component_descriptor_t *components;
  guint component_count;
  gboolean derived;               // TRUE for every v1 adapter
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_quantity_descriptor_t;

typedef struct dt_remote_quantity_module_adapter_t
{
  const char *operation;
  guint minimum_params_version, maximum_params_version;
  const dt_remote_quantity_descriptor_t *quantities;
  guint quantity_count;
  const char *const *native_fields;   // scalar params fields the write hook
  guint native_field_count;           // may touch; also the conflict set
} dt_remote_quantity_module_adapter_t;

const dt_remote_quantity_module_adapter_t *
dt_remote_quantity_registry_lookup(const char *operation, guint params_version);
gboolean dt_remote_quantity_registry_validate(const dt_remote_quantity_module_adapter_t *adapter,
                                              const struct dt_iop_module_so_t *so,
                                              dt_remote_error_t **error);
gboolean dt_remote_quantity_list_schema(const struct dt_iop_module_so_t *so,
                                        GPtrArray **out_fields,  // dt_remote_quantity_schema_t*
                                        dt_remote_error_t **error);
gboolean dt_remote_quantity_read_values(const struct dt_iop_module_t *module,
                                        const void *params,
                                        GHashTable **out,        // name → dt_remote_quantity_value_t*
                                        dt_remote_error_t **error);
gboolean dt_remote_quantity_apply_patch(const struct dt_iop_module_t *module,
                                        const void *old_params, void *new_params,
                                        const dt_remote_patch_t *patch,
                                        dt_remote_error_t **error);
/* thin wrapper mirroring dt_remote_band_apply_patch: null checks,
   own-class partition into a borrowed slice, then apply_entries */
gboolean dt_remote_quantity_apply_entries(const struct dt_iop_module_t *module,
                                          const void *old_params, void *new_params,
                                          GPtrArray *entries,
                                          const dt_remote_patch_t *patch, /* for the
                                          scalar-conflict check; may be NULL in
                                          pure-engine tests */
                                          dt_remote_error_t **error);
gboolean dt_remote_quantity_validate(const dt_remote_quantity_descriptor_t *desc,
                                     const GPtrArray *values
                                     /* dt_remote_quantity_component_value_t* */,
                                     dt_remote_error_t **error);  // pure

/* test seams, mirroring the band registry's lookup override */
typedef gboolean (*dt_remote_quantity_read_hook_t)(struct dt_iop_module_t *,
                                                   const void *, double *, size_t);
typedef gboolean (*dt_remote_quantity_write_hook_t)(struct dt_iop_module_t *,
                                                    const double *, size_t, void *);
void dt_remote_quantity_registry_set_lookup_override(
    const dt_remote_quantity_module_adapter_t *(*fn)(const char *, guint));
void dt_remote_quantity_set_hooks_override(dt_remote_quantity_read_hook_t read,
                                           dt_remote_quantity_write_hook_t write);
```

```c
/* remote_parameters.h — schema/value types, mirroring the band ones */
typedef struct dt_remote_quantity_schema_component_t
{
  char *name; char *unit;              // owned; unit nullable
  double minimum, maximum;
} dt_remote_quantity_schema_component_t;

typedef struct dt_remote_quantity_schema_t
{
  char *name; char *display_name; char *description;  // owned
  GPtrArray *components;   // dt_remote_quantity_schema_component_t*, owned
  gboolean derived;
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;       // owned, nullable
  dt_remote_parameter_condition_t *writable_when;     // owned, nullable
} dt_remote_quantity_schema_t;
void dt_remote_quantity_schema_free(dt_remote_quantity_schema_t *schema);

typedef struct dt_remote_quantity_value_t
{
  char *name;
  GPtrArray *values;       // dt_remote_quantity_component_value_t*, descriptor order
  gboolean active, effective, writable_now;
} dt_remote_quantity_value_t;
void dt_remote_quantity_value_free(dt_remote_quantity_value_t *value);
```

The semantic schema/value wrappers gain `quantity` union arms and
`dt_remote_semantic_schema_wrap_quantity` /
`dt_remote_semantic_value_wrap_quantity`.

**Behavioral requirements** (each is a unit test in Step 1):

1. `registry_validate` fails closed on: missing `so->remote_quantity_read`
   or `so->remote_quantity_write` (when no hooks override is set);
   version out of the pinned range; a component with
   `minimum >= maximum`; duplicate IDs within the adapter; an ID also
   registered by the curve, vector, or band registry for the same op
   (call all three lookups); a `native_fields` name that does not
   resolve to a writable scalar `float` leaf in the op's introspection;
   `component_count == 0` or `> DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP`.
2. `list_schema` converts descriptors in registry order from static data
   only (no hook call, no params); ops with no adapter yield
   `*out_fields = NULL`, return TRUE (silent degrade).
3. `read_values` calls the read hook (override when set) with a
   component-count-sized double buffer and stamps
   `active`/`writable_now` from the predicates (copy the band engine's
   evaluation); a hook returning FALSE is a
   `DT_REMOTE_ERR_INTERNAL`-family failure, never a crash.
4. `dt_remote_quantity_validate` is pure: reject a values list whose
   name set is not EXACTLY the descriptor's component set (missing →
   constraint `"missing_component"`, unknown → `"unknown_component"`,
   duplicate → `"duplicate_component"`); reject non-finite
   (`"non_finite"`) and out-of-domain (`"domain"`, tested at the double
   boundary: maximum 25000.0, value 25000.00001 → reject; exact bound →
   accept). Details carry `parameter` and (when attributable)
   `component` = the component name.
5. `apply_entries`: unknown ID → `DT_REMOTE_ERR_UNKNOWN_FIELD`;
   validation per requirement 4; **native-conflict rule**: when at least
   one quantity entry targets an adapter and the given `patch`'s
   `scalar_values` contains any entry whose `name` is in the adapter's
   `native_fields` → `DT_REMOTE_ERR_INVALID_VALUE`, constraint
   `"native_conflict"`, blob untouched; writability predicate evaluated
   against `new_params` (band-engine ordering); the write hook runs
   against `new_params` and afterward a **byte-check** verifies every
   byte outside the resolved `native_fields` offsets is unchanged —
   any stray byte → `DT_REMOTE_ERR_INTERNAL` and the caller's temp-blob
   rollback discards the write.
6. With the hooks override installed (a fake pair that scales/offsets a
   designated float field), a full apply → read round-trip returns the
   written component values.

- [ ] **Step 1: Write failing unit tests** in `test_remote_quantity.c`
  for requirements 1–6, using a test-local static adapter against the
  real loaded `temperature` `.so` (reuse `test_remote_band.c`'s
  module-loading fixture, `test_remote_band.c:677-690`) with the fake
  hooks override for every success-path test (the real hooks fail
  without GUI — that itself is one test: no override + real so →
  apply fails closed, blob unchanged). Also the Task 2 presence checks:
  `dt_iop_get_module_so("temperature")->remote_quantity_read != NULL`,
  `dt_iop_get_module_so("exposure")->remote_quantity_read == NULL`.
  Register the binary in `CMakeLists.txt` by copying the
  `test_remote_band` block.
- [ ] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R remote_quantity`
  Expected: FAIL (nothing implemented).
- [ ] **Step 3: Implement** `remote_quantity.h/.c`,
  `remote_quantity_registry.c` (empty `s_adapters[]` + lookup + validate
  + both override seams), the schema/value types + frees in
  `remote_parameters.h/.c`, and the wrapper quantity arms. Copy
  structure from the band twins; do not invent new patterns. Add both
  `.c` files to the CMake source list.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_quantity: engine and registry core (module-hook conversion, native-conflict rule)`

### Task 4: Quantity into the dispatch table; serializers; capability; represented_by

Spec §Wire and MCP contract. After this task any registered quantity
adapter is live end-to-end.

**Files:**
- Modify: `src/control/remote_edit.c` — quantity row appended to
  `s_class_ops[]` (`remote_edit.c:1036`; order curve, vector, bands,
  quantity) + `_annotate_quantity_represented_by`
- Modify: `src/control/remote_protocol.c` — serializers + hello
  capability (find the array: `grep -n "band_params" src/control/remote_protocol.c`)
- Modify: `src/tests/unittests/control/fixtures/hello_response.json` —
  append `"quantity_params"`
- Test: `src/tests/unittests/control/test_remote_edit.c`,
  `test_remote_protocol.c`

**Interfaces (Consumes):** everything Tasks 1–3 produced. Note
`dt_remote_quantity_apply_patch` matches the `apply_patch` row signature
(it takes the full patch, passes it through to `apply_entries` for the
conflict check).

- [ ] **Step 1: Write failing tests.**
  - `test_remote_protocol.c`: hello advertises `"quantity_params"`
    (copy the band capability test); serializer tests — a quantity
    schema serializes `class:"quantity"`, `derived`, `components` array
    with `unit` present only when non-NULL, `minimum`/`maximum`; a
    quantity value serializes
    `{class, active, effective, writable_now, values: {name: number}}`.
  - `test_remote_edit.c` (with the Task 3 lookup + hooks overrides
    installed, temperature as the fixture op): `get_module_schema`
    includes the quantity field AND the four coeff scalars keep
    `writable: true` while carrying
    `represented_by: ["wb.temperature"]` (assert both — this is the
    coexistence exception); a mutation with a quantity entry applies
    through the dispatch table and reads back via the fake hooks; a
    mixed scalar+quantity request touching a NON-native scalar
    (`temperature` has none besides the coeffs and denylisted `preset`,
    so use the fixture adapter's declared native list minus one — give
    the test adapter native_fields {"red","green","blue"} and write
    `various` as the disjoint scalar) applies atomically; the conflict
    case (scalar `red` + quantity entry) rejects `invalid_value`
    constraint `native_conflict` with the blob byte-identical.
- [ ] **Step 2: Run to verify failure.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R "remote_protocol|remote_edit"`
  Expected: FAIL.
- [ ] **Step 3: Implement.** `_quantity_schema_to_json` /
  `_quantity_value_to_json` beside the band serializers; wrapper-tag
  switch arms; the `"quantity_params"` capability string; the
  `s_class_ops[]` quantity row (`dt_remote_quantity_list_schema`,
  `dt_remote_quantity_read_values`, `dt_remote_quantity_apply_patch`,
  quantity wrap helpers, `_annotate_quantity_represented_by` — stamp
  each adapter `native_fields` root via the existing `_stamp_root`,
  which appends `represented_by` without touching writability, giving
  the coexistence behavior for free).
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS. Then
  `cd tools/mcp && .venv/bin/pytest -q` — PASS (the fixture gained a
  capability; the protocol test asserts list equality via the fixture
  itself, but eyeball for any hardcoded capability lists).
- [ ] **Step 5: Commit** — `remote_edit/protocol: quantity class live end-to-end (dispatch row, serializers, quantity_params)`

### Task 5: temperature adapter — wb.temperature

Spec §Class model. The shipped adapter is static data; the conversion
correctness itself is Task 8's job.

**Files:**
- Modify: `src/control/remote_quantity_registry.c`
- Test: `src/tests/unittests/control/test_remote_quantity.c`

**Descriptor data (normative):** op `temperature`, version pinned
min==max==4; one descriptor: name `wb.temperature`, display_name
`"white balance"`, derived TRUE, no predicates; components in order:
`{ "temperature", "kelvin", 1901.0, 25000.0 }`,
`{ "tint", NULL, 0.135, 2.326 }`; native_fields
`{ "red", "green", "blue", "various" }` (4).

- [ ] **Step 1: Write failing per-adapter tests** (mirror the band
  per-adapter style): registry lookup by op+version 4 succeeds, version
  3 fails; `registry_validate` passes against the real loaded `.so`
  (hooks present — no override needed for validate); schema lists one
  field `wb.temperature` with both components, units, domains, and
  `derived: true`; with the fake hooks override, a valid pair write
  round-trips and out-of-domain kelvin (25000.5) / tint (0.1) reject
  with constraint `domain` and component names `temperature` / `tint`;
  `preset` (denylisted since m1, `remote_edit.c:386`) is untouched by
  the coexistence work — schema still marks it unwritable.
- [ ] **Step 2: Run to verify failure.**
  Run: `ctest --test-dir build -R remote_quantity` — FAIL (no adapter).
- [ ] **Step 3: Implement the adapter table entry** as static data.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_quantity: temperature adapter (wb.temperature, Kelvin/tint)`

### Task 6: negadoctor and colorharmonizer vector adapters

Spec §Ride-along adapters. Pure static data on the m4 engine.

**Files:**
- Modify: `src/control/remote_vector_registry.c`
- Test: `src/tests/unittests/control/test_remote_vector.c`

**Descriptor data (normative):**

| op | name | native | components | native_capacity | range | subtype | predicates |
|---|---|---|---|---|---|---|---|
| `negadoctor` v2 | `dmin` | `Dmin` | 3 (`red`,`green`,`blue`) | 4 | [0.00001, 1.5] | `color`, color_space `display_rgb` | none |
| | `wb_high` | `wb_high` | 3 (`red`,`green`,`blue`) | 4 | [0.25, 2.0] | plain `vector` | none |
| | `wb_low` | `wb_low` | 3 (`red`,`green`,`blue`) | 4 | [0.25, 2.0] | plain `vector` | none |
| `colorharmonizer` v1 | `custom_hue` | `custom_hue` | 4 (`node1`..`node4`) | 4 | [0.0, 1.0] | plain `vector` | `writable_when: rule == DT_COLORHARMONIZER_CUSTOM` |
| | `node_saturation` | `node_saturation` | 4 (`node1`..`node4`) | 4 | [0.0, 2.0] | plain `vector` | none |

Both adapters: no `prepare_fields`, no `validate_completed`. Versions
pinned negadoctor min==max==2, colorharmonizer min==max==1. Before
writing the predicate, verify the enum member's exact introspection name
with `grep -n "DT_COLORHARMONIZER_CUSTOM" src/iop/colorharmonizer.c` and
follow the colorbalance `writable_when` row's predicate encoding exactly
(same file, existing rows). `num_custom_nodes` stays an ordinary
writable scalar; the vector is always written whole (4 components).

- [ ] **Step 1: Write failing per-adapter tests** (mirror the
  borders/watermark tests): lookup by op+version, wrong version fails;
  `registry_validate` against the real loaded `.so` (this also proves
  the 3-of-4 padded mapping validates); schema names/subtypes/ranges/
  color_space; read-back of defaults (negadoctor all three default 1.0
  per component, `negadoctor.c:73-78`; colorharmonizer `custom_hue`
   0.0×4, `node_saturation` 1.0×4, `colorharmonizer.c:77-79`); a valid
  write on `dmin` round-trips and byte 4 of the native array (the
  padding component) is unchanged; out-of-range rejects; on
  colorharmonizer, a `custom_hue` write under the default rule
  (`DT_COLORHARMONIZER_COMPLEMENTARY`) rejects `unsupported_field`, and
  the same write with `rule` set to CUSTOM in the same patch (projected
  params — the colorbalance precedent) applies; `node_saturation`
  writes regardless of rule.
- [ ] **Step 2: Run to verify failure.**
  Run: `ctest --test-dir build -R remote_vector` — FAIL (no adapters).
- [ ] **Step 3: Implement both adapter tables** following the existing
  registry layout exactly.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `ctest --test-dir build` — all suites PASS.
- [ ] **Step 5: Commit** — `remote_vector: negadoctor and colorharmonizer adapters`

### Task 7: Python sidecar — `quantities` argument

Spec §Wire and MCP contract (as amended). Mirrors the m5 `bands` task.

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`
- Test: `tools/mcp/tests/test_tools.py`, `tools/mcp/tests/test_protocol.py`

**Interfaces (Produces):**

```python
def _wire_quantity_values(quantities: dict[str, Any]) -> dict[str, Any]:
    """Translate the `quantities` tool argument to wire semantic_values
    entries. Tool shape: {name: {component: number}}. Mirrors
    _wire_band_values: validate fully before any wire traffic, raise
    ToolError on bad shapes."""
```

Wire entry produced: `{"class": "quantity", "values": {name: float}}`.

- [ ] **Step 1: Write failing sidecar tests** (copy the m5 bands test
  block wholesale, adapted):
  - `_wire_quantity_values` accepts
    `{"wb.temperature": {"temperature": 5500, "tint": 1.0}}`; rejects
    (ToolError): non-dict spec, empty dict, non-finite/boolean/
    non-numeric component values, non-string keys.
  - `set_module_params(quantities=...)` merges entries into
    `semantic_values` beside curves/vectors/bands; the overlap check
    becomes four-way (extend the parametrized three-way test with
    quantity pairings).
  - Capability gate: quantities given but `quantity_params` absent →
    ToolError mentioning `quantity_params` and "upgrade darktable"
    (copy the band gate test).
  - `test_protocol.py`: add
    `assert "quantity_params" in client.capabilities` to the
    capabilities test (the fixture gained it in Task 4).
- [ ] **Step 2: Run to verify failure.**
  Run: `cd tools/mcp && .venv/bin/pytest -q` — FAIL.
- [ ] **Step 3: Implement.** `_wire_quantity_values`; a
  `quantities: dict[str, Any] | None = None` parameter wired through the
  same translate/merge/overlap/capability pattern (the `translated`
  dict in `set_module_params` gains a `"quantities"` row; the gate adds
  the `quantity_params` branch); docstring paragraph modeled on the
  `bands` one, including the coeff-conflict warning (writing
  `red`/`green`/`blue`/`various` in `values` together with
  `wb.temperature` is rejected server-side) and the lossy-readback
  note.
- [ ] **Step 4: Run tests to verify pass.**
  Run: `cd tools/mcp && .venv/bin/pytest -q` — PASS.
- [ ] **Step 5: Commit** — `mcp sidecar: quantities argument (white balance et al)`

### Task 8: Live integration gates

Spec §Testing (integration). One file, mirroring
`test_bands_milestone5.py`'s structure and helpers (read it first).

**Files:**
- Create: `tools/mcp/tests/integration/test_quantity_milestone6.py`

**Gates** (each is one test):

1. hello advertises `quantity_params`.
2. `get_module_schema("temperature")`: `wb.temperature` with both
   components (units, domains), `derived: true`; `red`/`green`/`blue`/
   `various` still `writable: true` AND carrying
   `represented_by: ["wb.temperature"]`; `preset` unwritable.
3. Kelvin/tint write round-trips: write
   `{"temperature": 5500.0, "tint": 1.0}` via `semantic_values`, print
   the readback deltas, assert `abs(kelvin - 5500) <= 5.0` and
   `abs(tint - 1.0) <= 0.01`; render-diff vs before (temperature is
   enabled by default on raw files — assert `enabled` in the read, and
   pass `enable: true` if the fixture image has it off).
4. Pure-multiplier write still works (no regression): scalar write to
   `red` alone round-trips exactly.
5. Conflict: one request with scalar `red` + `wb.temperature` entry →
   `invalid_value` with constraint `native_conflict` in details; state
   unchanged.
6. Component-domain rejection: kelvin 26000 → `invalid_value` with
   `component: "temperature"`, constraint `domain`; state unchanged.
7. undo after a quantity write restores the prior coeffs (copy the band
   undo gate's structure — compare the four coeff scalars, which are
   exact, not the lossy derived pair).
8. stale-revision quantity write rejected (copy the band stale gate).
9. negadoctor `dmin` write round-trips with a render-diff (enable the
   module in the same request).
10. colorharmonizer `custom_hue` gated: write under default rule →
    `unsupported_field`; same write with `values: {"rule":
    "DT_COLORHARMONIZER_CUSTOM"}` in the same request applies and
    round-trips; `node_saturation` writes without the rule switch.
11. mixed request: temperature quantity entry + a disjoint module's
    edit is NOT required (cross-module patches don't exist); instead:
    negadoctor `dmin` + scalar `gamma` in one call → one history item
    (history-length check, m5 precedent).

- [ ] **Step 1: Write all gates.**
- [ ] **Step 2: Run the integration suite.**
  Run: `cd tools/mcp && DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`
  Expected: all gates PASS (plus the pre-existing OpenCL skip). If gate
  3's printed deltas exceed the pinned tolerances, STOP and re-derive
  the tolerance from the printed values (update the plan's pin and the
  spec's Testing note) rather than loosening silently.
- [ ] **Step 3: Run everything.**
  Run: `cmake --build build -j$(nproc) && ctest --test-dir build && cd tools/mcp && .venv/bin/pytest -q`
  Expected: all PASS.
- [ ] **Step 4: Commit** — `mcp tests: milestone 6 live integration gates (quantity + ride-alongs)`

### Task 9: Documentation

Spec §Documentation — same milestone, not after. Match by content if
line numbers have drifted.

**Files:**
- Modify: `docs/superpowers/specs/2026-07-16-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `tools/mcp/README.md`, `tools/mcp/docs/remote-control.md`
- Modify: `docs/superpowers/specs/2026-07-17-darktable-mcp-milestone6-quantity-class-design.md` (status line → implemented)

- [ ] **Step 1: supported-operations.** Milestone table: add the m6 row
  (quantity class + the optional iop API hook mechanism + two vector
  ride-alongs; design doc link). Move `temperature` (Tier 2 → Tier 1,
  since m6, usage core*, replacing the "stored values are multipliers"
  caveat with the milestone-6 phrasing and the coeff-coexistence note),
  `negadoctor` (Tier 2 → Tier 1, rare) and `colorharmonizer`
  (Tier 2 → Tier 1, occasional). Update tier counts: Tier 1 54 → 57,
  Tier 2 11 → 8, Tier 3 stays 6. Update the "How support is determined"
  paragraph with the quantity class sentence. Update the tool-surface
  editing row (`… and semantic quantities via `quantities``). Appendix:
  `temperature | preset` row already exists — unchanged.
- [ ] **Step 2: protocol-reference.** `quantity_params` capability
  section beside `band_params`: normative schema/value/patch shapes
  from spec §Wire and MCP contract **including `"class": "quantity"` in
  the patch example**; the `{parameter, component, constraint}` error
  details; the lossy-readback note; the coefficient-coexistence
  exception (natives stay writable + `native_conflict`); error
  precedence extended to curve → vector → bands → quantity.
- [ ] **Step 3: MCP user docs.** README: `quantities` section after the
  bands section — worked `wb.temperature` example, the conflict
  warning, the lossy-readback sentence, plus one-line mentions of the
  negadoctor/colorharmonizer vector additions in the vectors paragraph.
  remote-control.md: quantity capability bullet after the bands bullet.
- [ ] **Step 4: Design doc status** → implemented (with any in-flight
  amendments recorded in the status line — the m5 discipline).
- [ ] **Step 5: Verify all suites one final time** (full ctest + pytest
  + integration) and commit —
  `darktable-mcp: milestone 6 docs — tiers, quantity protocol, sidecar usage`

---

## Self-review notes (already applied)

- Spec coverage: class model + wire (T1/T4), conversion authority
  (T2/T3), coexistence/conflict (T3/T4/T8), ride-alongs (T6), sidecar
  amendment (T7), integration incl. tolerance/undo/stale/regression
  (T8), docs incl. precedence and the coexistence exception (T9).
- Type consistency: `dt_remote_quantity_*` names identical across Tasks
  1–5; the hook typedefs in Task 3 match the `OPTIONAL` signatures in
  Task 2 (`dt_iop_params_t*` erases to `void*`/`const void*` at the
  seam — the engine casts, as the band engine does for blobs).
- Deliberately not planned: no second quantity adapter, no hue
  periodicity, no `monochrome`/`colorcorrection`/`rawprepare` work, no
  reset_module special-casing (whole-params reset already covers
  derived values).
