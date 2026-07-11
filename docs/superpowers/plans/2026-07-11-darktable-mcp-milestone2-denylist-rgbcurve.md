# darktable MCP Milestone 2: Field Denylist + rgbcurve Semantic Curves — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enforce the per-module `writable: false` field denylist, then implement semantic curve editing for `rgbcurve` end-to-end (C engine → wire protocol → MCP sidecar) behind `semantic_params`/`curve_params` capabilities.

**Architecture:** Part A wires the existing (tested, unused) denylist mechanism into the two production call sites with a static per-op table. Part B follows the curve-classes low-level design steps 1–7: neutral semantic types, a bounds-checked introspection cursor, a common curve validator, a static registry with one `rgbcurve` adapter, capability-gated schema/value wire extensions, semantic patches folded into the existing atomic mutation transaction, and sidecar exposure through the existing `set_module_params` tool.

**Tech Stack:** C (GLib, cmocka, json-glib), Python 3.10+ (FastMCP sidecar, pytest), shared JSON fixtures under `src/tests/unittests/control/fixtures/`.

## Normative documents

- `docs/superpowers/specs/2026-07-11-darktable-mcp-milestone2-denylist-rgbcurve-design.md` — milestone scope and resolved decisions (this plan implements it).
- `docs/superpowers/specs/2026-07-05-darktable-mcp-curve-classes-low-level-design.md` — **normative for all Part B internals.** Where a step says "copy from curve design §X", the C declarations in that section are complete and final; copy them verbatim. Do not re-derive them.
- `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md` — appendix is the denylist source of truth (verify each entry against `src/iop/` before committing it).

## Global Constraints

- Protocol `protocol_version` stays **1**; all wire changes are additive and capability-gated. Server hello gains capabilities `"semantic_params"` and `"curve_params"`.
- MCP tool count stays **12**; curves ride on `set_module_params`.
- All live-module access and mutation runs on the main/GUI thread via the existing transaction; `remote_curve*` files never include JSON, socket, or MCP headers; never call GUI callbacks while building the temporary params block.
- Never clamp, sort, deduplicate, insert boundary points, or drop points implicitly. Validation failures mutate nothing and fail the whole request.
- Every heap-returned neutral object has a paired `_free` function; no returned object retains live-params or introspection pointers.
- Registry mismatch fails closed: semantic support for that op is disabled with an `internal` error; primitive support is unaffected.
- Wire error codes are reused, never invented: `unknown_field` (unknown semantic ID), `unsupported_field` (denylisted / class unimplemented / condition unsatisfied), `invalid_value` (malformed points/bounds/order/spacing/interpolation), `internal` (registry drift).
- C suites: `cmake --build build -- -j$(nproc)` then `ctest --test-dir build -R '<suite>' --output-on-failure`. Python: `cd tools/mcp && .venv/bin/pytest -q` (unit) / `.venv/bin/pytest -m integration -q` (live; needs `build/bin/darktable` + Xvfb).
- Commit after every green step; end every commit message with the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer.

---

## Part A — per-module field denylist

### Task 1: Denylist table and schema-path enforcement

**Files:**
- Modify: `src/control/remote_edit.c` (table + lookup + call site at `dt_remote_get_module_schema`, currently `remote_edit.c:757`)
- Modify: `src/control/remote_edit.h` (declare lookup)
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md` (appendix corrections found during verification)
- Test: `src/tests/unittests/control/test_remote_edit.c`

**Interfaces:**
- Consumes: existing `dt_remote_denylist_t` (`remote_edit.h:146`), `dt_remote_denylisted()` (`remote_edit.c:341`), `dt_remote_schema_from_introspection(linear, denylist)`.
- Produces: `const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op);` — returns `NULL` (deny nothing) for ops without an entry. Task 2 and Task 9 call this.

- [ ] **Step 1: Verify the appendix against current `src/iop/` source**

For each row of the supported-operations appendix, confirm the field still exists with that name in the module's params struct (e.g. `grep -n "version" src/iop/filmicrgb.c` inside `dt_iop_filmicrgb_params_t`). Note corrections; fields that no longer exist are dropped from the table and struck from the appendix in Step 7. Do not add new fields beyond the appendix.

- [ ] **Step 2: Write the failing tests**

In `test_remote_edit.c` (follow the file's existing cmocka fixture pattern for schema tests):

```c
static void test_denylist_for_op_lookup(void **state)
{
  (void)state;
  const dt_remote_denylist_t *dl = dt_remote_denylist_for_op("filmicrgb");
  assert_non_null(dl);
  assert_true(dt_remote_denylisted(dl, "version"));
  assert_true(dt_remote_denylisted(dl, "spline_version"));
  assert_false(dt_remote_denylisted(dl, "white_point_source"));
  assert_null(dt_remote_denylist_for_op("exposure"));   // no entry: deny nothing
  assert_null(dt_remote_denylist_for_op(NULL));
}

static void test_schema_marks_denylisted_fields_unwritable(void **state)
{
  // Uses the same loaded-module harness the existing schema tests use.
  // filmicrgb "version" passes the scalar type filter but must come back
  // writable == FALSE and still be PRESENT in the schema.
  dt_remote_module_schema_t *schema = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_get_module_schema("filmicrgb", &schema, &err));
  gboolean found = FALSE;
  for(guint i = 0; i < schema->fields->len; i++)
  {
    dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
    if(!g_strcmp0(f->name, "version"))
    {
      found = TRUE;
      assert_false(f->writable);
    }
  }
  assert_true(found);
  dt_remote_module_schema_free(schema);
}
```

- [ ] **Step 3: Run to verify failure**

`cmake --build build -- -j$(nproc) && ctest --test-dir build -R test_remote_edit --output-on-failure`
Expected: link/compile failure on `dt_remote_denylist_for_op` for the first test; after stubbing the declaration, `test_schema_marks_denylisted_fields_unwritable` fails on `assert_false(f->writable)`.

- [ ] **Step 4: Implement the table and wire the schema path**

In `remote_edit.c`, next to `dt_remote_denylisted()`:

```c
// Per-op forced-writable:false table. Source of truth:
// docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md
// (appendix); every entry verified against src/iop/ at commit time.
typedef struct dt_remote_op_denylist_t
{
  const char *op;
  dt_remote_denylist_t denylist;
} dt_remote_op_denylist_t;

#define DENY(...) { .names = (const char *const[]){ __VA_ARGS__, NULL } }

static const dt_remote_op_denylist_t s_op_denylists[] = {
  { "ashift",          DENY("cl", "cr", "ct", "cb",
                            "last_drawn_lines_count", "last_drawn_lines_version",
                            "last_quad_lines") },
  { "channelmixerrgb", DENY("x", "y", "version") },
  { "colorcontrast",   DENY("a_offset", "b_offset", "unbound") },
  { "colorize",        DENY("version") },
  { "colorzones",      DENY("splines_version") },
  { "crop",            DENY("ratio_n", "ratio_d") },
  { "denoiseprofile",  DENY("fix_anscombe_and_nlmeans_norm", "use_new_vst",
                            "wb_adaptive_anscombe") },
  { "dither",          DENY("palette", "random.radius", "random.range") },
  { "filmicrgb",       DENY("version", "spline_version") },
  { "highlights",      DENY("blendL", "blendC") },
  { "lens",            DENY("crop", "focal", "aperture", "distance",
                            "has_been_set", "md_version", "reserved") },
  { "lowpass",         DENY("unbound") },
  { "overlay",         DENY("imgid", "dummy0", "dummy1", "dummy2") },
  { "relight",         DENY("center") },
  { "shadhi",          DENY("reserved2", "flags", "low_approximation") },
  { "temperature",     DENY("preset") },
  { "tonecurve",       DENY("tonecurve_preset", "tonecurve_unbound_ab") },
  { "vignette",        DENY("unbound") },
};

const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op)
{
  if(!op) return NULL;
  for(size_t i = 0; i < G_N_ELEMENTS(s_op_denylists); i++)
    if(!strcmp(s_op_denylists[i].op, op)) return &s_op_denylists[i].denylist;
  return NULL;
}
```

Exact `ashift` field names and dotted `dither` names must match what Step 1 found in the introspection linear listing (dotted names for nested `random.*`); adjust the initializer to the verified names. Then change line 757:

```c
schema->fields = dt_remote_schema_from_introspection(linear, dt_remote_denylist_for_op(op));
```

Declare in `remote_edit.h` next to the `dt_remote_denylist_t` typedef:

```c
/** static per-op forced-writable:false table (supported-operations appendix).
 * NULL for ops with no entry -- meaning deny nothing. */
const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op);
```

- [ ] **Step 5: Run to verify pass**

`cmake --build build -- -j$(nproc) && ctest --test-dir build -R test_remote_edit --output-on-failure`
Expected: PASS, including all pre-existing tests.

- [ ] **Step 6: Run the full C suite set**

`ctest --test-dir build -R 'test_remote_' --output-on-failure`
Expected: all suites PASS (schema fixtures that asserted writability of a now-denylisted field must be updated to the new truth, not deleted).

- [ ] **Step 7: Update the appendix and commit**

Apply the Step 1 corrections to the appendix (same commit — the spec requires table and appendix to move together).

```bash
git add src/control/remote_edit.[ch] src/tests/unittests/control/test_remote_edit.c \
        docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md
git commit -m "remote_edit: enforce the per-op writable:false denylist in schemas"
```

### Task 2: Denylist enforcement on the mutation path + shared fixture + sidecar/live coverage

**Files:**
- Modify: `src/control/remote_edit.c` (call site in `dt_remote_set_module_params`, currently `remote_edit.c:894`)
- Create: `src/tests/unittests/control/fixtures/set_module_params_error_denylisted_field_request.json`
- Create: `src/tests/unittests/control/fixtures/set_module_params_error_denylisted_field_response.json`
- Test: `src/tests/unittests/control/test_remote_edit.c`, `src/tests/unittests/control/test_remote_protocol.c`, `tools/mcp/tests/test_tools.py`, `tools/mcp/tests/integration/test_editing.py`

**Interfaces:**
- Consumes: `dt_remote_denylist_for_op()` from Task 1; existing `dt_remote_patch_apply(linear, denylist, patch, blob, error)`.
- Produces: mutation-path rejection with wire code `unsupported_field`; the two fixtures, consumed byte-identically by C dispatcher tests and sidecar tests.

- [ ] **Step 1: Write the failing C test**

```c
static void test_set_module_params_rejects_denylisted_field(void **state)
{
  // Same live-darkroom harness as the existing set_module_params tests.
  // A patch naming filmicrgb.version must fail whole with
  // DT_REMOTE_ERR_UNSUPPORTED_FIELD and leave live params byte-identical.
  ...build patch with one entry: name="version", value=3...
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_set_module_params(&ref, patch, &result, &err));
  assert_int_equal(err->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  ...assert live params block unchanged (memcmp against pre-copy)...
}
```

(The `...` lines follow the exact harness idiom of the neighboring `test_set_module_params_*` tests in the same file — copy their setup/teardown verbatim.)

- [ ] **Step 2: Run to verify failure**

`ctest --test-dir build -R test_remote_edit --output-on-failure`
Expected: FAIL — the write currently succeeds because line 894 passes NULL.

- [ ] **Step 3: Wire the call site**

At `remote_edit.c:894`, inside `dt_remote_set_module_params` (which knows the module's op via the resolved instance `module->op`):

```c
if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op(module->op), patch, temp_params, error))
```

- [ ] **Step 4: Run to verify pass, then add the shared fixtures**

`ctest --test-dir build -R test_remote_edit --output-on-failure` → PASS.

`set_module_params_error_denylisted_field_request.json`:

```json
{
  "id": 22,
  "method": "set_module_params",
  "params": {
    "module": "filmicrgb",
    "instance": 0,
    "values": { "version": 3 }
  }
}
```

`set_module_params_error_denylisted_field_response.json`:

```json
{
  "id": 22,
  "ok": false,
  "error": {
    "code": "unsupported_field",
    "message": "field 'version' is not writable",
    "retryable": false
  }
}
```

Add a dispatcher fixture test in `test_remote_protocol.c` following the existing `set_module_params_error_*` fixture-pair pattern in that file, and a sidecar test in `tools/mcp/tests/test_tools.py`:

```python
async def test_set_module_params_denylisted_field_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "set_module_params", "set_module_params_error_denylisted_field_response.json"
    )
    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params", {"module": "filmicrgb", "values": {"version": 3}}
        )
    message = str(excinfo.value)
    assert "unsupported_field" in message
    assert "writable flag" in message  # errors.py hint for unsupported_field
```

- [ ] **Step 5: Run C + Python suites**

`ctest --test-dir build -R 'test_remote_' --output-on-failure` → PASS.
`cd tools/mcp && .venv/bin/pytest -q` → PASS.

- [ ] **Step 6: Add the live integration assertion**

In `tools/mcp/tests/integration/test_editing.py`, extend the schema-reading test (or add one) with:

```python
async def test_denylisted_fields_read_only_live(darktable_session):
    """filmicrgb bookkeeping fields are advertised non-writable and rejected."""
    async with harness.connected_client(darktable_session) as client:
        schema = await client.call("get_module_schema", {"module": "filmicrgb"})
        by_name = {f["name"]: f for f in schema["fields"]}
        assert by_name["version"]["writable"] is False
        with pytest.raises(ProtocolError) as excinfo:
            await client.call(
                "set_module_params",
                {"module": "filmicrgb", "instance": 0, "values": {"version": 3}},
            )
        assert excinfo.value.code == "unsupported_field"
```

- [ ] **Step 7: Run live integration (needs build + Xvfb), then commit**

`cd tools/mcp && .venv/bin/pytest -m integration tests/integration/test_editing.py -q` → PASS.

```bash
git add src/control/remote_edit.c src/tests/unittests/control/ tools/mcp/tests/
git commit -m "remote_edit: enforce the denylist on the mutation path"
```

---

## Part B — rgbcurve semantic-curve slice

### Task 3: Neutral semantic types and patch extension

**Files:**
- Create: `src/control/remote_parameters.h`, `src/control/remote_parameters.c`
- Modify: `src/control/remote_edit.h` (extend `dt_remote_patch_t`), `src/CMakeLists.txt` (add the new C file to `SOURCE_FILES`/`HEADER_FILES` — the source list is explicit, not globbed)
- Test: `src/tests/unittests/control/test_remote_curve.c` (new), `src/tests/unittests/control/CMakeLists.txt`

**Interfaces:**
- Produces (verbatim from curve design §Neutral semantic types and §Threading and ownership; these exact names are used by Tasks 4–10): `dt_remote_parameter_class_t`, `dt_remote_writability_t`, `dt_remote_curve_interpolation_t`, `dt_remote_curve_endpoint_policy_t`, `dt_remote_spacing_rule_t`, `dt_remote_predicate_operator_t`, `dt_remote_parameter_condition_t`, `dt_remote_curve_axis_t`, `dt_remote_curve_schema_t`, `dt_remote_curve_point_t`, `dt_remote_curve_value_t`, `dt_remote_curve_patch_t`, `dt_remote_semantic_patch_t`, and `dt_remote_curve_schema_free` / `dt_remote_curve_value_free` / `dt_remote_semantic_patch_free`.
- `dt_remote_patch_t` gains `GPtrArray *semantic_values; /* dt_remote_semantic_patch_t, nullable */` — existing callers pass NULL semantics unchanged.

- [ ] **Step 1: Register the new cmocka suite** — add `test_remote_curve` to `src/tests/unittests/control/CMakeLists.txt` copying the `test_remote_edit` block (including the WIN32 copy stanza).
- [ ] **Step 2: Write failing tests** — construct each neutral object, exercise its `_free` on fully- and partially-populated instances (NULL-safe), and assert `dt_remote_patch_t` with `semantic_values == NULL` still passes the existing "at least one scalar/enable" requirement while a patch with only semantic values also counts as non-empty.
- [ ] **Step 3: Run to verify failure** — `ctest --test-dir build -R test_remote_curve` fails to compile.
- [ ] **Step 4: Implement** — copy the declarations from curve design §Neutral semantic types into `remote_parameters.h` verbatim; implement constructors/destructors in `remote_parameters.c` (g_new0/g_free/g_array_unref discipline, NULL-tolerant frees). Extend `dt_remote_patch_t` and update its existing free/validate helpers ("values must be non-empty" check at `remote_edit.c:547` becomes: empty means no scalars AND no semantics AND no enable).
- [ ] **Step 5: Run to verify pass** — `ctest --test-dir build -R 'test_remote_(curve|edit)' --output-on-failure` → PASS.
- [ ] **Step 6: Commit** — `git commit -m "remote_parameters: neutral semantic types and patch extension"`.

### Task 4: Introspection cursor

**Files:**
- Create: `src/control/remote_curve.h`, `src/control/remote_curve.c` (cursor lives here; registry comes in Task 6)
- Modify: `src/CMakeLists.txt`
- Test: `src/tests/unittests/control/test_remote_curve.c`

**Interfaces:**
- Consumes: `dt_introspection_get_child()`, `dt_introspection_access_array()` (`src/common/introspection.h`); path types from curve design §Introspection path (copy verbatim into `remote_curve.h`).
- Produces:

```c
gboolean dt_remote_path_resolve(const dt_remote_introspection_path_t *path,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                const dt_introspection_field_t **out_field,
                                void **out_ptr,
                                dt_remote_error_t **error);
```

  Returns the leaf introspection field and a pointer into `params_blob`; retains neither. Errors are `DT_REMOTE_ERR_INTERNAL` (registry paths are compiled-in; a bad path is registry drift, never caller error).

- [ ] **Step 1: Write failing tests** (curve design §Unit tests / Introspection cursor is the checklist): resolve `curve_nodes[0][3].x` and `curve_num_nodes[1]` against a stack `dt_iop_rgbcurve_params_t` fixture via the loaded module SO's introspection; assert the returned pointer targets the fixture blob (`(char*)out_ptr - (char*)fixture` within `sizeof` bounds); reject wrong-type segment, out-of-bounds index, and missing child with `DT_REMOTE_ERR_INTERNAL`.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** — iterate segments, `DT_REMOTE_PATH_FIELD` → `dt_introspection_get_child`, `DT_REMOTE_PATH_INDEX` → `dt_introspection_access_array`, verifying header type before each descent and bounds before each index.
- [ ] **Step 4: Run to verify pass.**
- [ ] **Step 5: Commit** — `git commit -m "remote_curve: bounds-checked introspection path cursor"`.

### Task 5: Common curve validator

**Files:**
- Modify: `src/control/remote_curve.[ch]`
- Test: `src/tests/unittests/control/test_remote_curve.c`

**Interfaces:**
- Consumes: descriptor types from curve design §Semantic descriptor (copied in Task 6 — for this task declare `dt_remote_curve_descriptor_t` in `remote_curve.h` verbatim from the design; Task 6 populates instances).
- Produces:

```c
gboolean dt_remote_curve_validate(const dt_remote_curve_descriptor_t *desc,
                                  const GArray *points, /* dt_remote_curve_point_t */
                                  gboolean has_interpolation,
                                  dt_remote_curve_interpolation_t interpolation,
                                  dt_remote_error_t **error);
```

- [ ] **Step 1: Write failing tests** — one test per rule in curve design §Validation algorithm items 3–10, driven by a hand-built descriptor (2–20 points, spacing 0.0025 GREATER_THAN, strict order, optional boundaries, all-interpolations mask): under-/over-count, NaN, +inf, out-of-domain x and y, descending x, duplicate x, spacing exactly 0.0025 (rejected: rule is strictly-greater), spacing 0.00251 (accepted), disallowed interpolation vs mask, endpoints off the domain boundary accepted under `OPTIONAL` policy. Every rejection returns `DT_REMOTE_ERR_INVALID_VALUE` with `parameter`/`point_index`/`constraint` detail members and must not modify the points array.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** the checks in the §Validation algorithm order; comparisons honor `dt_remote_spacing_rule_t` (AT_LEAST = `>=`, GREATER_THAN = `>`); periodic wrap check only when the descriptor declares it (rgbcurve does not — cover via a synthetic periodic descriptor in tests).
- [ ] **Step 4: Run to verify pass.**
- [ ] **Step 5: Commit** — `git commit -m "remote_curve: common control-point curve validator"`.

### Task 6: Registry, rgbcurve descriptor, read-only engine

**Files:**
- Create: `src/control/remote_curve_registry.c`
- Modify: `src/control/remote_curve.[ch]`, `src/CMakeLists.txt`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c` (new; register in `CMakeLists.txt` like Task 3 Step 1)

**Interfaces:**
- Consumes: cursor (Task 4), validator (Task 5), neutral types (Task 3).
- Produces (verbatim signatures from curve design §Registry lifecycle and §Curve engine API):

```c
const dt_remote_curve_module_adapter_t *
dt_remote_curve_registry_lookup(const char *operation, guint params_version);

gboolean dt_remote_curve_registry_validate(const dt_remote_curve_module_adapter_t *adapter,
                                           const dt_introspection_t *introspection,
                                           dt_remote_error_t **error);

gboolean dt_remote_curve_list_schema(const dt_iop_module_so_t *module_so,
                                     GPtrArray **out, /* dt_remote_curve_schema_t */
                                     dt_remote_error_t **error);

gboolean dt_remote_curve_read_values(const dt_iop_module_t *module,
                                     const void *params,
                                     GHashTable **out, /* name -> dt_remote_curve_value_t */
                                     dt_remote_error_t **error);
```

- [ ] **Step 1: Write failing registry tests** — lookup `("rgbcurve", 1)` returns the adapter; unknown op / wrong version returns NULL; `dt_remote_curve_registry_validate` passes against the real loaded `rgbcurve` introspection and fails (with `DT_REMOTE_ERR_INTERNAL`) against a synthetic introspection where the node array is the wrong type; validation result is cached per `(op, version)`.
- [ ] **Step 2: Write failing read-path tests** — with a params fixture in automatic-RGB mode, `read_values` exposes `curve.master` `active/writable_now == TRUE` and `curve.red/green/blue` `active == FALSE` (all four always present, per the milestone spec's resolved decision 4); in manual mode the polarity flips; channel-0 points round-trip under `curve.master`; unused native capacity (indices ≥ `curve_num_nodes[ch]`) is never serialized; interpolation ints map to the three names.
- [ ] **Step 3: Run to verify failure.**
- [ ] **Step 4: Implement** — the `rgbcurve` adapter table in `remote_curve_registry.c` per curve design §Initial registry mapping / `rgbcurve` (four descriptors over channels 0/0/1/2; predicates on `curve_autoscale` vs `DT_S_SCALE_MANUAL_RGB` by enum name; domain [0,1]²; 2–20 points; spacing 0.0025 GREATER_THAN; boundary policy OPTIONAL; all three interpolations; `prepare_fields = {"curve_autoscale", "compensate_middle_grey"}`; `prepare`/`validate_completed` stubs returning TRUE for now — Task 9 fills them). Native layout paths: nodes `curve_nodes[ch]`, count `curve_num_nodes[ch]`, type `curve_type[ch]`, x/y fields `"x"`/`"y"`, no internal version. Implement `list_schema` (descriptor → owned schema copies) and `read_values` (cursor reads, predicate evaluation against the supplied params).
- [ ] **Step 5: Run to verify pass** — `ctest --test-dir build -R 'test_remote_curve' --output-on-failure`.
- [ ] **Step 6: Commit** — `git commit -m "remote_curve: registry, rgbcurve descriptors, read-only engine"`.

### Task 7: Wire read path — capabilities, schema and value responses

**Files:**
- Modify: `src/control/remote_protocol.c` (hello capabilities; `get_module_schema`/`get_module_params` serializers), `src/control/remote_edit.[ch]` (thread curve schema/values into the existing result structs)
- Create fixtures: `src/tests/unittests/control/fixtures/get_module_schema_rgbcurve_response.json`, `get_module_params_rgbcurve_response.json`
- Test: `src/tests/unittests/control/test_remote_protocol.c`, `tools/mcp/tests/test_protocol.py`

**Interfaces:**
- Consumes: `dt_remote_curve_list_schema` / `dt_remote_curve_read_values` (Task 6).
- Produces: hello `capabilities` gains `"semantic_params", "curve_params"`; `get_module_schema` result gains optional `semantic_fields` (shape: curve design §Schema response, including `represented_by` on the native array fields); `get_module_params` result gains optional `semantic_values` (shape: §Value response — points as `{"x":…,"y":…}` objects, uppercase interpolation names). Ops without a registry adapter emit neither member (byte-identical to today — all existing fixtures stay valid).

- [ ] **Step 1: Write the two response fixtures** exactly matching curve design §Schema response / §Value response for `rgbcurve` (identity 2-point master curve, automatic mode), and failing dispatcher tests asserting (a) hello advertises both new capabilities, (b) rgbcurve schema/params responses match the fixtures byte-for-byte, (c) an `exposure` schema response is unchanged from its existing fixture.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** serialization in `remote_protocol.c` (json-glib builders alongside the existing field serializers; condition triples serialize as `{"field": …, "not_equals"/"equals": …}`).
- [ ] **Step 4: Run C suites; then sidecar fixture passthrough tests** in `tools/mcp/tests/test_tools.py`: `get_module_schema`/`get_module_params` return the rgbcurve fixtures' `result` verbatim (the read path needs no sidecar code change — verify, don't modify).
- [ ] **Step 5: Run** `ctest --test-dir build -R 'test_remote_' --output-on-failure` and `cd tools/mcp && .venv/bin/pytest -q` → PASS.
- [ ] **Step 6: Commit** — `git commit -m "remote_protocol: capability-gated semantic schema and value responses"`.

### Task 8: Mutation engine — adapter callbacks and apply path

**Files:**
- Modify: `src/control/remote_curve.[ch]`, `src/control/remote_curve_registry.c` (real `prepare`/`validate_completed`), `src/control/remote_edit.c` (transaction steps 5–7 of curve design §Transaction integration)
- Test: `src/tests/unittests/control/test_remote_curve.c`, `test_remote_edit.c`

**Interfaces:**
- Consumes: everything above.
- Produces (verbatim from curve design §Curve engine API):

```c
gboolean dt_remote_curve_apply_patch(const dt_iop_module_t *module,
                                     const void *old_params,
                                     void *new_params,
                                     const dt_remote_patch_t *patch,
                                     dt_remote_error_t **error);
```

  called from `dt_remote_set_module_params` after scalar writes, before whole-block validation; on any failure the temporary block is discarded (existing transaction semantics).

- [ ] **Step 1: Write failing adapter/transaction tests** — the curve design §Unit tests / `rgbcurve` adapter and §Transaction lists are the test inventory; implement each named case as one cmocka test. The two transition cases pin the resolved decisions: entering manual RGB copies channel 0 into untouched G/B before explicit patches apply; a middle-grey compensation change with no work profile available fails with `DT_REMOTE_ERR_UNSUPPORTED_FIELD` (details `constraint: "work_profile_unavailable"`) mutating nothing (build the no-profile case by pointing the test develop at an image with no work profile set, mirroring how existing edit tests build their darkroom fixture).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** — `apply_patch`: resolve semantic ID → descriptor (unknown → `DT_REMOTE_ERR_UNKNOWN_FIELD`; wrong class / predicate unsatisfied against **projected** params → `DT_REMOTE_ERR_UNSUPPORTED_FIELD`), run Task 5 validator, run adapter `prepare` when a curve is present or a `prepare_fields` scalar changed, write points/count/type through the cursor (zero unused capacity — rgbcurve declares padding insignificant), then adapter `validate_completed`. The rgbcurve `prepare` reproduces the two storage transitions from curve design §Initial registry mapping using introspection accessors only (reference `rgbcurve.c` `gui_changed()` for the transition math; middle-grey transform goes through `dt_ioppr_get_pipe_work_profile_info` on the current pipe — fail `UNSUPPORTED_FIELD` with the `work_profile_unavailable` constraint detail when it returns no profile). Wire into `dt_remote_set_module_params` between scalar apply and block validation.
- [ ] **Step 4: Run to verify pass** — all `test_remote_*` suites.
- [ ] **Step 5: Commit** — `git commit -m "remote_curve: rgbcurve adapter callbacks and atomic apply path"`.

### Task 9: Wire mutation path — `semantic_values` parsing and dispatch

**Files:**
- Modify: `src/control/remote_protocol.c` (parse/validate `semantic_values` request member; read-back serialization)
- Create fixtures: `set_module_params_curve_request.json` / `set_module_params_curve_response.json`, `set_module_params_error_curve_invalid_spacing_request.json` / `..._response.json`, `set_module_params_error_curve_unknown_id_request.json` / `..._response.json`
- Test: `src/tests/unittests/control/test_remote_protocol.c`

**Interfaces:**
- Consumes: `dt_remote_semantic_patch_t` (Task 3), transaction (Task 8).
- Produces: `set_module_params` accepts optional `semantic_values` (shape: curve design §Mutation request — `class` required, exactly-`x`/`y` point objects, unknown members rejected, duplicate IDs rejected, point-list cap 64 entries pre-engine); success responses include read-back `semantic_values` for every written entry. Requests without `semantic_values` are byte-identical to today.

- [ ] **Step 1: Write the six fixtures** (success: 4-point `curve.master` MONOTONE_HERMITE patch mirroring §Mutation request; spacing error: two points at x 0.5000/0.5020 → `invalid_value` with `point_index`/`constraint` details; unknown ID: `curve.alpha` → `unknown_field`) and failing dispatcher tests driving each request fixture and asserting the paired response.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** parsing per curve design §Serialization rules (finite doubles, exact member sets, `class: "curve"` required) into `dt_remote_semantic_patch_t`, dispatch through the extended transaction, serialize read-back.
- [ ] **Step 4: Run** all C suites → PASS.
- [ ] **Step 5: Commit** — `git commit -m "remote_protocol: semantic_values mutation parsing and read-back"`.

### Task 10: Sidecar — `curves` argument, capability gating

**Files:**
- Modify: `tools/mcp/src/darktable_mcp/server.py`, `tools/mcp/src/darktable_mcp/protocol.py` (expose hello capabilities on the client)
- Test: `tools/mcp/tests/test_tools.py`, `tools/mcp/tests/test_protocol.py`

**Interfaces:**
- Consumes: hello `capabilities` (Task 7), wire `semantic_values` (Task 9).
- Produces: `set_module_params` tool gains `curves: dict[str, Any] | None = None`. Tool-arg shape (milestone spec) translates to wire shape: `{"curve.master": {"points": [[0.0,0.0],[0.4,0.5],[1.0,1.0]], "interpolation": "cubic_spline"}}` becomes `semantic_values` entries with `"class": "curve"`, point pairs converted to `{"x":…,"y":…}` objects, interpolation upper-cased and validated against `{CUBIC_SPLINE, CATMULL_ROM, MONOTONE_HERMITE}`. `ProtocolClient` records the hello `capabilities` list as `client.capabilities`.

- [ ] **Step 1: Write failing tests** — (a) `curves` translation: fake server sees `semantic_values` with class/objects/uppercase names and no `curves` member; (b) omission: no `curves` → no `semantic_values` on the wire; (c) capability gating: fake server hello without `curve_params` → tool raises a `ToolError` mentioning `curve_params` **without any wire call** to `set_module_params`; (d) invalid interpolation name → client-side `ToolError` before any wire call; (e) exact-tool-list test still passes at twelve; (f) `test_protocol.py`: `client.capabilities` populated from hello.
- [ ] **Step 2: Run to verify failure** — `cd tools/mcp && .venv/bin/pytest -q`.
- [ ] **Step 3: Implement** — in `server.py`, extend the Task-10 tool with the translation helper and gate:

```python
_INTERPOLATIONS = {"CUBIC_SPLINE", "CATMULL_ROM", "MONOTONE_HERMITE"}

def _wire_semantic_values(curves: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for name, spec in curves.items():
        entry: dict[str, Any] = {
            "class": "curve",
            "points": [{"x": x, "y": y} for x, y in spec["points"]],
        }
        interp = spec.get("interpolation")
        if interp is not None:
            interp = str(interp).upper()
            if interp not in _INTERPOLATIONS:
                raise ValueError(
                    f"unknown interpolation {interp!r}; expected one of "
                    + ", ".join(sorted(_INTERPOLATIONS))
                )
            entry["interpolation"] = interp
        out[name] = entry
    return out
```

  and in the tool body, after `_client()`: if `curves` is given and `"curve_params" not in client.capabilities`, raise the connection-level error type the other tools use, with the message `"this darktable does not advertise curve_params; upgrade darktable to edit curves"`. Update the tool docstring with the invariants block from the milestone spec (2–20 points, strict ascending x, spacing > 0.0025, stored coordinates, whole-curve replacement, read-back points come as `{x, y}` objects).
- [ ] **Step 4: Run to verify pass** — full `pytest -q`.
- [ ] **Step 5: Commit** — `git commit -m "darktable-mcp: curves member on set_module_params with capability gating"`.

### Task 11: Live integration, CI, and docs

**Files:**
- Create: `tools/mcp/tests/integration/test_curves.py`
- Modify: `.github/workflows/mcp.yml` (suite-count comment + any suite enumeration), `tools/mcp/README.md`, `tools/mcp/docs/ubuntu-claude-code-setup.md`, `tools/mcp/docs/remote-control.md` (one paragraph on curve capability), `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md` (record: capability-gated optional request members do not bump `protocol_version`; add both capabilities and the `semantic_values` member shapes), `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md` (rgbcurve row: semantic curve support noted)
- Test: the new integration module

**Interfaces:**
- Consumes: everything above; the existing `harness.connected_client` / `darktable_session` fixtures.

- [ ] **Step 1: Write the integration tests** — one test per acceptance-gate item, following curve design §Integration tests 1–7 and the milestone spec's testing list (steps 8–9, CPU/OpenCL and collapsed-GUI variants, are folded into the round-trip test via parametrization where the harness supports it; otherwise assert the CPU path and file the OpenCL variant as a skip-marked test with the reason string `"needs opencl-capable CI runner"`):
  - `test_rgbcurve_schema_and_identity_roundtrip` — schema advertises the four semantic IDs; identity read-back has 2 points, no native leakage (no `curve_nodes` in `semantic_values`).
  - `test_curve_patch_advances_revision_and_preview` — apply the 4-point §Mutation request curve with `expected_revision`; read-back matches; `render_preview` bytes differ from the pre-edit render.
  - `test_scalar_curve_enable_is_one_history_item` — combined patch adds exactly one `get_history` item.
  - `test_undo_restores_identity_and_mode` — one `undo` restores exact prior points and `curve_autoscale`.
  - `test_manual_mode_transition_atomicity` — switch to manual RGB + replace `curve.red` in one request; `curve.green`/`curve.blue` show the copied channel-0 identity; linked-mode write to `curve.green` fails `unsupported_field` with nothing changed.
  - `test_denylisted_and_stale_revision_untouched` — stale `expected_revision` with a curve patch changes nothing.
- [ ] **Step 2: Run live** — `cd tools/mcp && .venv/bin/pytest -m integration -q` (rebuild `build/` first: the C side changed). Expected: PASS.
- [ ] **Step 3: Update CI and docs** — bump the C-suite count comment in `mcp.yml` (11 → 13: `test_remote_curve`, `test_remote_curve_registry`); README tool table row for `set_module_params` mentions `curves`; Ubuntu guide §10 gains a curve example; protocol reference records the no-version-bump rule, both capabilities, and the request/response member shapes; supported-operations `rgbcurve` row notes semantic support.
- [ ] **Step 4: Full verification** — `ctest --test-dir build -R 'test_remote_' --output-on-failure`, `pytest -q`, `pytest -m integration -q`: all PASS with pristine output.
- [ ] **Step 5: Commit** — `git commit -m "darktable-mcp: rgbcurve live integration gates, CI suites, and docs"`.

---

## Self-review record

- **Spec coverage:** Part A → Tasks 1–2 (table, both call sites, fixture, sidecar hint, live assertion, appendix corrections). Part B steps 1–7 → Tasks 3 (types), 4 (cursor), 5 (validator), 6 (registry/read engine), 7 (wire read + capabilities), 8–9 (transaction + wire mutation), 10 (sidecar), 11 (acceptance gate, CI, docs, protocol-reference resolution of the version-bump question). Resolved decisions 1–7 of the milestone spec are pinned in Tasks 8 (fail-closed no-profile, transitions), 7/10 (no bump, capability gating), 6 (inactive-always-returned, central registry), 10 (no new tool).
- **Type consistency:** all Part B names are copied from the curve design's normative declarations; the only plan-invented names are `dt_remote_denylist_for_op`, `dt_remote_op_denylist_t`, `dt_remote_path_resolve`, `dt_remote_curve_validate`, `_wire_semantic_values`, and `client.capabilities`, each defined once in its producing task and referenced with identical spelling afterward.
- **Known judgment calls for implementers:** exact `ashift`/`dither` denylist field spellings come from Step 1 source verification, not this plan; the OpenCL integration variant may land as a skip-marked test if the CI runner lacks OpenCL.
