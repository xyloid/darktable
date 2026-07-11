# darktable MCP Milestone 3 — Curve Adapters for tonecurve, colorzones, basecurve — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the milestone-2 semantic curve machinery to the three remaining
modules the curve-classes design already mapped — `tonecurve`, `colorzones`,
and `basecurve` — so their curves become writable through `set_module_params`'
`semantic_values`, with zero engine or protocol changes.

**Architecture:** Milestone 2 built a generic engine
(`src/control/remote_curve.c`: path resolution, pure validator, apply engine)
and a registry (`src/control/remote_curve_registry.c`) where each module is
pure static data plus at most two callbacks. `represented_by` stamping, schema
listing, value reads, capability advertisement, the JSON protocol, and the
Python sidecar are all already generic — verified this session. Milestone 3
therefore only adds registry descriptor tables, three small adapter callbacks
(colorzones), unit tests against the real loaded module `.so`s, live
integration gates, and documentation. The normative per-module mapping is
`docs/superpowers/specs/2026-07-05-darktable-mcp-curve-classes-low-level-design.md`
§"Initial registry mapping" (tonecurve/colorzones/basecurve subsections).

**Tech Stack:** C (GLib, darktable introspection), cmocka unit tests, Python
3 + FastMCP sidecar (no code change expected), pytest integration harness
under Xvfb.

## Global Constraints

- Work in the git worktree `<REPO>/.claude/worktrees/mcp-remote-edit`, branch `worktree-mcp-remote-edit`. All paths below are relative to that root.
- TDD: every behavior change lands with its failing test first.
- Commit messages end with the two trailers:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_013WEQMKDwcviZRWSK2ugzTH`.
- **No engine changes**: `src/control/remote_curve.c`, `remote_edit.c`, `remote_protocol.c`, `remote_parameters.*` are not modified. If a task appears to require it, stop and escalate — it means a design assumption broke.
- **No protocol or capability change**: `protocol_version` stays `1`; the hello already advertises `semantic_params` + `curve_params` statically; per the recorded resolved decision, new curve-bearing ops are additive and need no new capability.
- **No Python sidecar code change**: `tools/mcp/src/darktable_mcp/` is generic over semantic IDs. Only docs change.
- Verification suite (all must pass at every commit): `ctest --test-dir build` (13 suites), `cd tools/mcp && .venv/bin/pytest -q` (91 unit tests), and for Task 5 also `DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q`.
- Native interpolation ints are `CUBIC_SPLINE=0, CATMULL_ROM=1, MONOTONE_HERMITE=2` (`src/common/curve_tools.h`); the registry maps them by explicit value identity, never cast.
- Params versions (verified via `DT_MODULE_INTROSPECTION`): `tonecurve` = 5, `colorzones` = 5, `basecurve` = 6. Adapters pin `minimum_params_version == maximum_params_version` to exactly these.

## Out of scope (do not implement)

- **`atrous`**: its curves are fixed-size parallel arrays (`float x[5][6]; float y[5][6]`) with no count/type leaves — they do not fit `dt_remote_native_curve_layout_t` (array-of-structs + count + type). Supporting it requires a second layout kind in the engine; that is a milestone-4 design task, not a registry entry.
- **`rgblevels`**: `float levels[3][3]` is black/grey/white triples, not a control-point curve. It needs a future "levels" semantic class.
- Both exclusions get documented in Task 6.

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/control/remote_curve_registry.c` | modify | shared adapter helpers (Task 1) + three new descriptor tables/adapters (Tasks 2–4); `s_adapters[]` grows from 1 to 4 entries |
| `src/tests/unittests/control/test_remote_curve_registry.c` | modify | schema/read-values/lookup tests per new module |
| `src/tests/unittests/control/test_remote_curve.c` | modify | fixture generalization + apply-path tests per new module |
| `tools/mcp/tests/integration/test_curves_milestone3.py` | create | live acceptance gates for the three modules |
| `tools/mcp/README.md`, `tools/mcp/docs/ubuntu-claude-code-setup.md`, `tools/mcp/docs/remote-control.md`, `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`, `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md` | modify | document the three new curve-bearing ops; tier moves |

No new C source files: the registry file's own comment ("a future op (e.g.
tonecurve) adds another entry here, not a parallel lookup mechanism") locks
the pattern in.

---

### Task 1: Extract shared adapter helpers from the rgbcurve-only code

Pure refactor — no behavior change, no new tests; the existing 13 suites
prove it. The rgbcurve section of `remote_curve_registry.c` contains four
helpers that are already generic (they take `ctx`/`desc` arguments and never
mention rgbcurve fields) plus a `validate_completed` body that every
milestone-3 module needs verbatim.

**Files:**
- Modify: `src/control/remote_curve_registry.c`

**Interfaces:**
- Produces (file-local, used by Tasks 2–4):
  - `static gboolean adapter_fail_internal(dt_remote_error_t **error, const char *message)`
  - `static gboolean adapter_resolve_top(const dt_remote_curve_context_t *ctx, void *params, const char *name, const dt_introspection_field_t **field, void **ptr, dt_remote_error_t **error)`
  - `static gboolean adapter_resolve_native(const dt_remote_curve_context_t *ctx, const dt_remote_curve_descriptor_t *desc, void *params, const dt_introspection_field_t **nodes_field, void **nodes_ptr, const dt_introspection_field_t **count_field, void **count_ptr, const dt_introspection_field_t **type_field, void **type_ptr, dt_remote_error_t **error)`
  - `static gboolean adapter_resolve_node(const dt_remote_curve_descriptor_t *desc, const dt_introspection_field_t *nodes_field, void *nodes_ptr, guint index, float **x, float **y)`
  - `static gboolean adapter_validate_active_curves(const struct dt_remote_curve_context_t *ctx, const void *new_params, dt_remote_error_t **error)`

- [ ] **Step 1: Rename the generic helpers**

In `src/control/remote_curve_registry.c`, rename (declaration, definition,
and every call site — `grep -n` each old name to find them all):

| old | new |
|---|---|
| `rgbcurve_fail_internal` | `adapter_fail_internal` |
| `rgbcurve_resolve_top` | `adapter_resolve_top` |
| `rgbcurve_resolve_native` | `adapter_resolve_native` |
| `rgbcurve_resolve_node` | `adapter_resolve_node` |
| `rgbcurve_validate_completed` | `adapter_validate_active_curves` |

Keep `rgbcurve_read_bool`, `rgbcurve_read_mode`, `rgbcurve_is_identity`,
`rgbcurve_copy_channel`, `rgbcurve_transform_channel`, and
`rgbcurve_prepare` under their rgbcurve names — they encode rgbcurve
semantics. In `adapter_validate_active_curves`, generalize the one
rgbcurve-specific error string:

```c
      return adapter_fail_internal(error, _("completed curve value disappeared from registry"));
```

(the old text was "rgbcurve completed value disappeared from registry").
Update `s_rgbcurve_adapter`'s member to
`.validate_completed = adapter_validate_active_curves,`.

- [ ] **Step 2: Build and run the full C suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build`
Expected: `100% tests passed, 0 tests failed out of 13`

- [ ] **Step 3: Commit**

```bash
git add src/control/remote_curve_registry.c
git commit -m "remote_curve_registry: extract shared adapter helpers from rgbcurve"
```

---

### Task 2: tonecurve adapter

Three semantic curves per the design doc: `curve.lightness` (native channel
0, always active/writable) and `curve.a`/`curve.b` (channels 1/2, active and
writable only when `tonecurve_autoscale_ab == DT_S_SCALE_MANUAL`). Facts
verified against `src/iop/tonecurve.c`: params v5;
`dt_iop_tonecurve_node_t tonecurve[3][20]` with float `x`/`y`;
`tonecurve_nodes[3]`; `tonecurve_type[3]` (`$DEFAULT: MONOTONE_HERMITE`);
autoscale enum tokens `DT_S_SCALE_MANUAL`(0) / `DT_S_SCALE_AUTOMATIC`(1) /
`DT_S_SCALE_AUTOMATIC_XYZ`(2) / `DT_S_SCALE_AUTOMATIC_RGB`(3, the default);
`tonecurve_preset`/`tonecurve_unbound_ab` already denylisted.

**Spacing decision (do not "fix" this to 0.0025):** tonecurve camera
presets ship adjacent node gaps down to ~0.00096 (e.g. the NIKON D5100
preset's L curve, `src/iop/tonecurve.c` preset table). Because
`adapter_validate_active_curves` re-validates *every* active curve of the
projected params, any nonzero spacing floor would reject MCP patches merely
because a factory preset is loaded. Descriptors therefore use
`adjacent_spacing_rule = DT_REMOTE_SPACING_NONE` with
`strict_x_order = TRUE` — strictly ascending x is the only spacing invariant
the module itself maintains.

**Files:**
- Modify: `src/control/remote_curve_registry.c`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c`, `src/tests/unittests/control/test_remote_curve.c`

**Interfaces:**
- Consumes: Task 1's `adapter_*` helpers.
- Produces: registry entries `s_tonecurve_adapter` (op `"tonecurve"`, versions 5..5), semantic IDs `"curve.lightness"`, `"curve.a"`, `"curve.b"`.

- [ ] **Step 1: Write the failing registry tests**

In `test_remote_curve_registry.c`, after the existing rgbcurve tests, add
(reusing that file's existing harness — the real module `.so` loads via
`dt_iop_get_module_so()` exactly as the rgbcurve tests do; mirror their
setup/teardown):

```c
static void test_registry_lookup_finds_tonecurve_only_at_version_5(void **state)
{
  (void)state;
  const dt_remote_curve_module_adapter_t *adapter = dt_remote_curve_registry_lookup("tonecurve", 5);
  assert_non_null(adapter);
  assert_string_equal(adapter->operation, "tonecurve");
  assert_int_equal(adapter->curve_count, 3);
  assert_null(dt_remote_curve_registry_lookup("tonecurve", 4));
  assert_null(dt_remote_curve_registry_lookup("tonecurve", 6));
}

static void test_registry_resolves_tonecurve_manual_enum_name(void **state)
{
  (void)state;
  // Same confirm-the-generated-token pattern as
  // test_registry_resolves_manual_rgb_enum_name: the predicate's enum_name
  // must be a real introspection name on the real loaded module.
  dt_iop_module_so_t *so = dt_iop_get_module_so("tonecurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);
  dt_introspection_field_t *field = NULL;
  guint8 dummy = 0;
  (void)dt_introspection_get_child(intro->field, &dummy, "tonecurve_autoscale_ab", &field);
  assert_non_null(field);
  int value = -1;
  assert_true(dt_introspection_get_enum_value(field, "DT_S_SCALE_MANUAL", &value));
  assert_int_equal(value, 0);
}

static void test_tonecurve_schema_lists_lightness_then_a_then_b(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("tonecurve");
  assert_non_null(so);
  GPtrArray *schemas = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_list_schema(so, &schemas, &error));
  assert_null(error);
  assert_int_equal(schemas->len, 3);

  const dt_remote_curve_schema_t *lightness = g_ptr_array_index(schemas, 0);
  const dt_remote_curve_schema_t *a = g_ptr_array_index(schemas, 1);
  const dt_remote_curve_schema_t *b = g_ptr_array_index(schemas, 2);
  assert_string_equal(lightness->name, "curve.lightness");
  assert_string_equal(a->name, "curve.a");
  assert_string_equal(b->name, "curve.b");
  assert_int_equal(lightness->writability, DT_REMOTE_WRITABLE_NOW);
  assert_null(lightness->active_when);
  assert_int_equal(a->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
  assert_int_equal(b->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
  assert_non_null(a->writable_when);
  assert_string_equal(a->writable_when->field, "tonecurve_autoscale_ab");
  assert_string_equal(a->writable_when->enum_name, "DT_S_SCALE_MANUAL");
  assert_int_equal(lightness->default_interpolation, DT_REMOTE_CURVE_MONOTONE_HERMITE);
  assert_false(lightness->periodic_x);
  g_ptr_array_unref(schemas);
}
```

Register the three tests in the file's `cmocka_unit_test` group list.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_curve_registry --output-on-failure`
Expected: FAIL — `dt_remote_curve_registry_lookup("tonecurve", 5)` returns NULL (no adapter yet).

- [ ] **Step 3: Write the failing apply-path tests**

In `test_remote_curve.c`, first generalize the fixture constructor so other
ops can reuse the whole helper family: change `rgbcurve_fixture_new(void)` to

```c
static rgbcurve_fixture_t *adapter_fixture_new(const char *op)
{
  rgbcurve_fixture_t *fixture = g_new0(rgbcurve_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;
  fixture->module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so(op);
  assert_non_null(so);
  assert_false(dt_iop_load_module(fixture->module, so, &fixture->dev));
  memcpy(fixture->module->params, fixture->module->default_params,
         fixture->module->params_size);
  fixture->dev.iop = g_list_append(fixture->dev.iop, fixture->module);
  return fixture;
}

static rgbcurve_fixture_t *rgbcurve_fixture_new(void)
{
  return adapter_fixture_new("rgbcurve");
}
```

and in `rgbcurve_apply_to_copy()` replace the hard-coded op with the
fixture's module op:

```c
  if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op(fixture->module->op), patch, projected, error))
```

(the helpers `rgbcurve_set_enum`, `rgbcurve_make_curve_patch`,
`rgbcurve_read_value`, `assert_curve_points`, `rgbcurve_patch_init/cleanup`
are already op-agnostic — reuse them as-is). Then add:

```c
static void test_tonecurve_adapter_ab_write_in_linked_mode_is_unsupported(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("tonecurve");
  // default params: tonecurve_autoscale_ab == DT_S_SCALE_AUTOMATIC_RGB

  const dt_remote_curve_point_t points[] = { { 0.0, 0.1 }, { 1.0, 0.9 } };
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.a", points, 2, FALSE, 0));

  void *projected = g_malloc0(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(error);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_tonecurve_adapter_mode_flip_and_ab_write_in_one_patch(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("tonecurve");

  const dt_remote_curve_point_t points[] = { { 0.0, 0.0 }, { 0.5, 0.4 }, { 1.0, 1.0 } };
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  rgbcurve_make_enum_entry(fixture, "tonecurve_autoscale_ab", "DT_S_SCALE_MANUAL"));
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.a", points, 3, TRUE,
                                            DT_REMOTE_CURVE_CATMULL_ROM));

  void *projected = g_malloc0(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  // conditions were evaluated against the projected (manual) mode
  GHashTable *values = NULL;
  dt_remote_curve_value_t *a = rgbcurve_read_value(fixture, projected, "curve.a", &values);
  assert_true(a->active);
  assert_true(a->writable_now);
  assert_curve_points(a, points, 3, 1e-6);
  assert_int_equal(a->interpolation, DT_REMOTE_CURVE_CATMULL_ROM);
  // lightness untouched and still active
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.lightness"))->active);
  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}
```

Register both in the group list.

- [ ] **Step 4: Run to verify they fail**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_remote_curve --output-on-failure`
Expected: the two new tonecurve tests FAIL (no adapter: patch containing
`curve.a` fails with `unknown semantic curve` / UNKNOWN_FIELD, so
`test_tonecurve_adapter_ab_write_in_linked_mode_is_unsupported` fails on the
error-code assertion and the mode-flip test fails on `assert_true`); the
refactored rgbcurve tests still PASS.

- [ ] **Step 5: Implement the tonecurve registry section**

In `remote_curve_registry.c`, after the rgbcurve adapter table and before
`s_adapters[]`, add:

```c
/* ---------------------------------------------------------------------- */
/* tonecurve descriptor table (curve-classes design doc SS Initial         */
/* registry mapping / tonecurve). Params v5: tonecurve[3][20] node         */
/* structs, tonecurve_nodes[3], tonecurve_type[3];                         */
/* tonecurve_autoscale_ab gates a/b exactly like rgbcurve's manual mode.   */
/* ---------------------------------------------------------------------- */

// Compact channel-path declaration for the milestone-3 adapters. The
// rgbcurve tables above predate it and stay longhand as committed.
#define ADAPTER_CHANNEL_PATH(var, field_name, channel)                       \
  static const dt_remote_path_segment_t var##_segments[] = {                 \
    { .type = DT_REMOTE_PATH_FIELD, .value.field = field_name },             \
    { .type = DT_REMOTE_PATH_INDEX, .value.index = channel },                \
  };                                                                         \
  static const dt_remote_introspection_path_t var = {                        \
    .segments = var##_segments, .length = G_N_ELEMENTS(var##_segments)       \
  };

ADAPTER_CHANNEL_PATH(s_tc_nodes_ch0, "tonecurve", 0)
ADAPTER_CHANNEL_PATH(s_tc_nodes_ch1, "tonecurve", 1)
ADAPTER_CHANNEL_PATH(s_tc_nodes_ch2, "tonecurve", 2)
ADAPTER_CHANNEL_PATH(s_tc_count_ch0, "tonecurve_nodes", 0)
ADAPTER_CHANNEL_PATH(s_tc_count_ch1, "tonecurve_nodes", 1)
ADAPTER_CHANNEL_PATH(s_tc_count_ch2, "tonecurve_nodes", 2)
ADAPTER_CHANNEL_PATH(s_tc_type_ch0, "tonecurve_type", 0)
ADAPTER_CHANNEL_PATH(s_tc_type_ch1, "tonecurve_type", 1)
ADAPTER_CHANNEL_PATH(s_tc_type_ch2, "tonecurve_type", 2)

// Confirmed against the loaded module by
// test_registry_resolves_tonecurve_manual_enum_name.
static const dt_remote_parameter_predicate_t s_tonecurve_manual_predicate = {
  .field = "tonecurve_autoscale_ab", .op = DT_REMOTE_PREDICATE_EQ, .enum_name = "DT_S_SCALE_MANUAL"
};

// Shared field values for one tonecurve descriptor. Camera presets ship
// adjacent gaps down to ~0.00096, so any nonzero spacing floor would make
// adapter_validate_active_curves() reject patches merely because a factory
// preset is loaded: spacing rule NONE, strict ascending only.
#define TONECURVE_DESCRIPTOR_COMMON                                          \
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },           \
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },           \
    .minimum_points = 2,                                                     \
    .maximum_points = 20,                                                    \
    .minimum_x_spacing = 0.0,                                                \
    .adjacent_spacing_rule = DT_REMOTE_SPACING_NONE,                         \
    .minimum_wrap_spacing = 0.0,                                             \
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,                             \
    .strict_x_order = TRUE,                                                  \
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,       \
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,                       \
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,               \
    .periodic_when = NULL

static const dt_remote_curve_descriptor_t s_tonecurve_curves[] = {
  {
    .name = "curve.lightness",
    .display_name_msgid = N_("L"),
    .description_msgid = NULL,
    .native = { .nodes = s_tc_nodes_ch0, .count = s_tc_count_ch0, .type = s_tc_type_ch0,
                .x_field = "x", .y_field = "y",
                .internal_version = s_no_internal_version_path, .internal_version_value = 0 },
    TONECURVE_DESCRIPTOR_COMMON,
    .active_when = NULL,
    .writable_when = NULL,
  },
  {
    .name = "curve.a",
    .display_name_msgid = N_("a"),
    .description_msgid = NULL,
    .native = { .nodes = s_tc_nodes_ch1, .count = s_tc_count_ch1, .type = s_tc_type_ch1,
                .x_field = "x", .y_field = "y",
                .internal_version = s_no_internal_version_path, .internal_version_value = 0 },
    TONECURVE_DESCRIPTOR_COMMON,
    .active_when = &s_tonecurve_manual_predicate,
    .writable_when = &s_tonecurve_manual_predicate,
  },
  {
    .name = "curve.b",
    .display_name_msgid = N_("b"),
    .description_msgid = NULL,
    .native = { .nodes = s_tc_nodes_ch2, .count = s_tc_count_ch2, .type = s_tc_type_ch2,
                .x_field = "x", .y_field = "y",
                .internal_version = s_no_internal_version_path, .internal_version_value = 0 },
    TONECURVE_DESCRIPTOR_COMMON,
    .active_when = &s_tonecurve_manual_predicate,
    .writable_when = &s_tonecurve_manual_predicate,
  },
};

// A scalar-only flip to manual mode activates a/b holding whatever prior
// state the params carry; validate_completed re-checks them then.
static const char *const s_tonecurve_prepare_fields[] = { "tonecurve_autoscale_ab" };

static const dt_remote_curve_module_adapter_t s_tonecurve_adapter = {
  .operation = "tonecurve",
  .minimum_params_version = 5,
  .maximum_params_version = 5,
  .curves = s_tonecurve_curves,
  .curve_count = G_N_ELEMENTS(s_tonecurve_curves),
  .prepare_fields = s_tonecurve_prepare_fields,
  .prepare_field_count = G_N_ELEMENTS(s_tonecurve_prepare_fields),
  .prepare = NULL,
  .validate_completed = adapter_validate_active_curves,
};
```

Note the field-order requirement: because `TONECURVE_DESCRIPTOR_COMMON` uses
designated initializers, listing `.active_when`/`.writable_when` after the
macro is valid C99 — keep that ordering.

Then extend the adapter table:

```c
static const dt_remote_curve_module_adapter_t *const s_adapters[] = {
  &s_rgbcurve_adapter,
  &s_tonecurve_adapter,
};
```

- [ ] **Step 6: Run all curve suites, then the full suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R 'test_remote_curve' --output-on-failure && ctest --test-dir build`
Expected: all new tests PASS; `100% tests passed, 0 tests failed out of 13`.

- [ ] **Step 7: Commit**

```bash
git add src/control/remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve.c
git commit -m "remote_curve_registry: tonecurve semantic curve adapter"
```

---

### Task 3: colorzones adapter

Three always-active curves `curve.lightness`/`curve.chroma`/`curve.hue`
(native output channels 0/1/2), periodic when the shared select-by axis is
hue. Facts verified against `src/iop/colorzones.c`: params v5;
`dt_iop_colorzones_node_t curve[3][20]`; `curve_num_nodes[3]`;
`curve_type[3]`; `channel` enum token `DT_IOP_COLORZONES_h` (value 2, the
default); `splines_version` int with `DT_IOP_COLORZONES_SPLINES_V2 == 1`
(new edits use V2); module spacing constant
`DT_IOP_COLORZONES_MIN_X_DISTANCE == 0.0025f`; defaults are 2 CATMULL_ROM
nodes per curve at y=0.5, x at 0.25/0.75 in hue mode and 0.0/1.0 otherwise
(`_reset_nodes`/`_reset_parameters`); the GUI resets all curves when
select-by changes (`gui_changed`).

Three design decisions, each mirroring the module or the design doc:

1. **Descriptors keep `wrap_spacing_rule = NONE`** even though hue mode is
   periodic: the pure validator applies wrap spacing unconditionally, and
   the same stored curves are legitimately edge-pinned (wrap gap exactly 0)
   under the non-hue axes — that is the module's own default. The adapter's
   `validate_completed` implements wrap-spacing only when select-by is hue,
   exactly as the design doc assigns ("the adapter … implements
   wrap-spacing validation"). Known residual caveat, to note in the code: a
   pre-existing GUI-drawn hue curve with points pinned to both edges would
   also fail the wrap check on a later MCP patch; if the Task 5 live gates
   surface real states like that, the agreed fallback is relaxing the wrap
   threshold to `> 0.0` — do not silently pick a third behavior.
2. **`internal_version` stamps `splines_version = 1` (V2)** on every curve
   write, via the engine's existing layout mechanism — the design doc's
   "always writes the current `splines_version`".
3. **`prepare` mirrors the GUI's reset-on-select-by-change**: when a patch
   changes `channel`, every output curve is reset to the new axis's default
   shape before explicit semantic curves in the same request overwrite
   theirs. Without this, curves keep coordinates from the old axis meaning
   and (worse) edge-pinned lightness-mode defaults become invalid periodic
   curves.

**Files:**
- Modify: `src/control/remote_curve_registry.c`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c`, `src/tests/unittests/control/test_remote_curve.c`

**Interfaces:**
- Consumes: Task 1 helpers; Task 2's `ADAPTER_CHANNEL_PATH` macro and test fixture `adapter_fixture_new`.
- Produces: `s_colorzones_adapter` (op `"colorzones"`, versions 5..5), IDs `"curve.lightness"`, `"curve.chroma"`, `"curve.hue"`.

- [ ] **Step 1: Write the failing registry tests**

In `test_remote_curve_registry.c` add:

```c
static void test_registry_lookup_finds_colorzones_only_at_version_5(void **state)
{
  (void)state;
  const dt_remote_curve_module_adapter_t *adapter = dt_remote_curve_registry_lookup("colorzones", 5);
  assert_non_null(adapter);
  assert_int_equal(adapter->curve_count, 3);
  assert_null(dt_remote_curve_registry_lookup("colorzones", 4));
}

static void test_colorzones_schema_all_writable_and_conditionally_periodic(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("colorzones");
  assert_non_null(so);
  GPtrArray *schemas = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_list_schema(so, &schemas, &error));
  assert_null(error);
  assert_int_equal(schemas->len, 3);
  const char *expected_names[] = { "curve.lightness", "curve.chroma", "curve.hue" };
  for(guint i = 0; i < 3; i++)
  {
    const dt_remote_curve_schema_t *schema = g_ptr_array_index(schemas, i);
    assert_string_equal(schema->name, expected_names[i]);
    assert_int_equal(schema->writability, DT_REMOTE_WRITABLE_NOW);
    assert_false(schema->periodic_x); // conditional, so not unconditionally periodic
    assert_non_null(schema->periodic_when);
    assert_string_equal(schema->periodic_when->field, "channel");
    assert_string_equal(schema->periodic_when->enum_name, "DT_IOP_COLORZONES_h");
    assert_int_equal(schema->default_interpolation, DT_REMOTE_CURVE_CATMULL_ROM);
  }
  g_ptr_array_unref(schemas);
}
```

- [ ] **Step 2: Write the failing apply-path tests**

In `test_remote_curve.c` add:

```c
static void test_colorzones_adapter_periodic_flag_follows_select_by(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("colorzones");
  // default select-by is hue: all three curves periodic, defaults 0.25/0.75
  GHashTable *values = NULL;
  dt_remote_curve_value_t *hue =
    rgbcurve_read_value(fixture, fixture->module->params, "curve.hue", &values);
  assert_true(hue->periodic_x);
  const dt_remote_curve_point_t hue_defaults[] = { { 0.25, 0.5 }, { 0.75, 0.5 } };
  assert_curve_points(hue, hue_defaults, 2, 1e-6);
  g_hash_table_unref(values);

  rgbcurve_set_enum(fixture, fixture->module->params, "channel", "DT_IOP_COLORZONES_L");
  values = NULL;
  hue = rgbcurve_read_value(fixture, fixture->module->params, "curve.hue", &values);
  assert_false(hue->periodic_x);
  g_hash_table_unref(values);
  rgbcurve_fixture_free(fixture);
}

static void test_colorzones_adapter_rejects_zero_wrap_gap_in_hue_mode(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("colorzones");

  const dt_remote_curve_point_t pinned[] = { { 0.0, 0.4 }, { 1.0, 0.6 } };
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.hue", pinned, 2, FALSE, 0));

  void *projected = g_malloc0(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(error->details_json);
  assert_non_null(strstr(error->details_json, "wrap_spacing"));
  dt_remote_error_free(error);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_colorzones_adapter_write_stamps_splines_version_v2(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("colorzones");
  // simulate a legacy V1 edit
  dt_introspection_field_t *version_field = NULL;
  int *version_ptr = rgbcurve_field_ptr(fixture, fixture->module->params,
                                        "splines_version", &version_field);
  *version_ptr = 0; // DT_IOP_COLORZONES_SPLINES_V1

  const dt_remote_curve_point_t points[] = { { 0.2, 0.45 }, { 0.8, 0.55 } };
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.chroma", points, 2, FALSE, 0));

  void *projected = g_malloc0(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);
  int *projected_version = rgbcurve_field_ptr(fixture, projected,
                                              "splines_version", &version_field);
  assert_int_equal(*projected_version, 1); // DT_IOP_COLORZONES_SPLINES_V2
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_colorzones_adapter_select_by_change_resets_curves(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("colorzones");
  // put a recognizable non-default shape on the hue curve first
  const dt_remote_curve_point_t custom[] = { { 0.3, 0.4 }, { 0.7, 0.6 } };
  dt_remote_patch_t warmup;
  rgbcurve_patch_init(&warmup);
  g_ptr_array_add(warmup.semantic_values,
                  rgbcurve_make_curve_patch("curve.hue", custom, 2, FALSE, 0));
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &warmup, fixture->module->params, &error));
  assert_null(error);
  rgbcurve_patch_cleanup(&warmup);

  // scalar-only select-by change: prepare must reset all output curves to
  // the new axis's edge-pinned defaults, mirroring the GUI
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  rgbcurve_make_enum_entry(fixture, "channel", "DT_IOP_COLORZONES_L"));
  void *projected = g_malloc0(fixture->module->params_size);
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *hue = rgbcurve_read_value(fixture, projected, "curve.hue", &values);
  const dt_remote_curve_point_t reset_defaults[] = { { 0.0, 0.5 }, { 1.0, 0.5 } };
  assert_curve_points(hue, reset_defaults, 2, 1e-6);
  assert_int_equal(hue->interpolation, DT_REMOTE_CURVE_CATMULL_ROM);
  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}
```

(`rgbcurve_field_ptr` returns `void *`; the `int *` assignments above rely
on the C implicit conversion the file already uses elsewhere.) Register all
tests from Steps 1–2 in their group lists.

- [ ] **Step 3: Run to verify the new tests fail**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R 'test_remote_curve' --output-on-failure`
Expected: new colorzones tests FAIL (no adapter → schema list empty /
unknown semantic curve); everything else PASSES.

- [ ] **Step 4: Implement the colorzones registry section**

After the tonecurve section in `remote_curve_registry.c`:

```c
/* ---------------------------------------------------------------------- */
/* colorzones descriptor table (curve-classes design doc SS Initial        */
/* registry mapping / colorzones). Params v5: curve[3][20] node structs,   */
/* curve_num_nodes[3], curve_type[3]; the `channel` enum ("select by")     */
/* picks the shared input x axis for all three output curves and makes     */
/* them periodic when it is hue.                                           */
/* ---------------------------------------------------------------------- */

ADAPTER_CHANNEL_PATH(s_cz_nodes_ch0, "curve", 0)
ADAPTER_CHANNEL_PATH(s_cz_nodes_ch1, "curve", 1)
ADAPTER_CHANNEL_PATH(s_cz_nodes_ch2, "curve", 2)
ADAPTER_CHANNEL_PATH(s_cz_count_ch0, "curve_num_nodes", 0)
ADAPTER_CHANNEL_PATH(s_cz_count_ch1, "curve_num_nodes", 1)
ADAPTER_CHANNEL_PATH(s_cz_count_ch2, "curve_num_nodes", 2)
ADAPTER_CHANNEL_PATH(s_cz_type_ch0, "curve_type", 0)
ADAPTER_CHANNEL_PATH(s_cz_type_ch1, "curve_type", 1)
ADAPTER_CHANNEL_PATH(s_cz_type_ch2, "curve_type", 2)

static const dt_remote_path_segment_t s_cz_version_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "splines_version" },
};
static const dt_remote_introspection_path_t s_cz_version_path = {
  .segments = s_cz_version_segments, .length = G_N_ELEMENTS(s_cz_version_segments)
};

static const dt_remote_parameter_predicate_t s_colorzones_hue_predicate = {
  .field = "channel", .op = DT_REMOTE_PREDICATE_EQ, .enum_name = "DT_IOP_COLORZONES_h"
};

// src/iop/colorzones.c: DT_IOP_COLORZONES_MIN_X_DISTANCE (0.0025f) and
// DT_IOP_COLORZONES_SPLINES_V2 (1) are private to the module; mirrored
// here and pinned by test_colorzones_adapter_write_stamps_splines_version_v2.
#define COLORZONES_MIN_X_DISTANCE 0.0025
#define COLORZONES_SPLINES_V2 1

// wrap_spacing_rule stays NONE: the same stored curves are legitimately
// edge-pinned (wrap gap exactly 0) under the non-hue select-by axes -- the
// module's own non-periodic defaults. colorzones_validate_completed()
// enforces the wrap gap only when select-by is hue.
#define COLORZONES_DESCRIPTOR_COMMON                                         \
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },           \
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },           \
    .minimum_points = 2,                                                     \
    .maximum_points = 20,                                                    \
    .minimum_x_spacing = COLORZONES_MIN_X_DISTANCE,                          \
    .adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN,                 \
    .minimum_wrap_spacing = 0.0,                                             \
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,                             \
    .strict_x_order = TRUE,                                                  \
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,       \
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,                       \
    .default_interpolation = DT_REMOTE_CURVE_CATMULL_ROM,                    \
    .active_when = NULL,                                                     \
    .writable_when = NULL,                                                   \
    .periodic_when = &s_colorzones_hue_predicate

static const dt_remote_curve_descriptor_t s_colorzones_curves[] = {
  {
    .name = "curve.lightness",
    .display_name_msgid = N_("lightness"),
    .description_msgid = NULL,
    .native = { .nodes = s_cz_nodes_ch0, .count = s_cz_count_ch0, .type = s_cz_type_ch0,
                .x_field = "x", .y_field = "y",
                .internal_version = s_cz_version_path,
                .internal_version_value = COLORZONES_SPLINES_V2 },
    COLORZONES_DESCRIPTOR_COMMON,
  },
  {
    .name = "curve.chroma",
    .display_name_msgid = N_("chroma"),
    .description_msgid = NULL,
    .native = { .nodes = s_cz_nodes_ch1, .count = s_cz_count_ch1, .type = s_cz_type_ch1,
                .x_field = "x", .y_field = "y",
                .internal_version = s_cz_version_path,
                .internal_version_value = COLORZONES_SPLINES_V2 },
    COLORZONES_DESCRIPTOR_COMMON,
  },
  {
    .name = "curve.hue",
    .display_name_msgid = N_("hue"),
    .description_msgid = NULL,
    .native = { .nodes = s_cz_nodes_ch2, .count = s_cz_count_ch2, .type = s_cz_type_ch2,
                .x_field = "x", .y_field = "y",
                .internal_version = s_cz_version_path,
                .internal_version_value = COLORZONES_SPLINES_V2 },
    COLORZONES_DESCRIPTOR_COMMON,
  },
};

static gboolean colorzones_read_channel(const dt_remote_curve_context_t *ctx,
                                        const void *params,
                                        int *out,
                                        int *hue_value,
                                        dt_remote_error_t **error)
{
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!adapter_resolve_top(ctx, (void *)params, "channel", &field, &ptr, error)) return FALSE;
  if(field->header.type != DT_INTROSPECTION_TYPE_ENUM
     || !dt_introspection_get_enum_value((dt_introspection_field_t *)field,
                                         "DT_IOP_COLORZONES_h", hue_value))
    return adapter_fail_internal(error, _("colorzones channel enum drifted from introspection"));
  *out = *(const int *)ptr;
  return TRUE;
}

// Mirrors _reset_parameters()/_reset_nodes() in src/iop/colorzones.c: two
// CATMULL_ROM nodes per output curve at y = 0.5, x pinned to the domain
// edges when the select-by axis is not periodic and pulled inside it
// (0.25/0.75) when it is.
static gboolean colorzones_reset_curves(const dt_remote_curve_context_t *ctx,
                                        void *params,
                                        gboolean periodic,
                                        dt_remote_error_t **error)
{
  for(guint i = 0; i < ctx->adapter->curve_count; i++)
  {
    const dt_remote_curve_descriptor_t *desc = &ctx->adapter->curves[i];
    const dt_introspection_field_t *nodes_field = NULL;
    const dt_introspection_field_t *count_field = NULL;
    const dt_introspection_field_t *type_field = NULL;
    void *nodes_ptr = NULL;
    void *count_ptr = NULL;
    void *type_ptr = NULL;
    if(!adapter_resolve_native(ctx, desc, params, &nodes_field, &nodes_ptr,
                               &count_field, &count_ptr, &type_field, &type_ptr, error))
      return FALSE;
    if(count_field->header.type != DT_INTROSPECTION_TYPE_INT
       || type_field->header.type != DT_INTROSPECTION_TYPE_INT)
      return adapter_fail_internal(error, _("colorzones channel layout drifted from introspection"));

    for(guint k = 0; k < 2; k++)
    {
      float *x = NULL;
      float *y = NULL;
      if(!adapter_resolve_node(desc, nodes_field, nodes_ptr, k, &x, &y))
        return adapter_fail_internal(error, _("colorzones node layout drifted from introspection"));
      *x = periodic ? (k ? 0.75f : 0.25f) : (k ? 1.0f : 0.0f);
      *y = 0.5f;
    }
    *(int *)count_ptr = 2;
    *(int *)type_ptr = 1; // CATMULL_ROM (src/common/curve_tools.h)
  }
  return TRUE;
}

static gboolean colorzones_prepare(const struct dt_remote_curve_context_t *ctx,
                                   const void *old_params,
                                   void *new_params,
                                   const dt_remote_patch_t *patch,
                                   dt_remote_error_t **error)
{
  (void)patch;
  int old_channel = 0;
  int new_channel = 0;
  int old_hue = 0;
  int new_hue = 0;
  if(!colorzones_read_channel(ctx, old_params, &old_channel, &old_hue, error)
     || !colorzones_read_channel(ctx, new_params, &new_channel, &new_hue, error))
    return FALSE;
  if(old_channel == new_channel) return TRUE;
  // The GUI resets every output curve when select-by changes (gui_changed()
  // in src/iop/colorzones.c) because the stored x axis changes meaning.
  // Mirror it on the projected block; explicit semantic curves in the same
  // request are written after prepare and overwrite these defaults.
  return colorzones_reset_curves(ctx, new_params, new_channel == new_hue, error);
}

static gboolean colorzones_validate_completed(const struct dt_remote_curve_context_t *ctx,
                                              const void *new_params,
                                              dt_remote_error_t **error)
{
  if(!adapter_validate_active_curves(ctx, new_params, error)) return FALSE;

  int channel = 0;
  int hue_value = 0;
  if(!colorzones_read_channel(ctx, new_params, &channel, &hue_value, error)) return FALSE;
  if(channel != hue_value) return TRUE;

  GHashTable *values = NULL;
  if(!dt_remote_curve_read_values(ctx->module, new_params, &values, error)) return FALSE;
  for(guint i = 0; i < ctx->adapter->curve_count; i++)
  {
    const dt_remote_curve_descriptor_t *desc = &ctx->adapter->curves[i];
    const dt_remote_curve_value_t *value = g_hash_table_lookup(values, desc->name);
    if(!value)
    {
      g_hash_table_unref(values);
      return adapter_fail_internal(error, _("completed curve value disappeared from registry"));
    }
    const guint n = value->points->len;
    if(n < 2) continue;
    const dt_remote_curve_point_t first = g_array_index(value->points, dt_remote_curve_point_t, 0);
    const dt_remote_curve_point_t last = g_array_index(value->points, dt_remote_curve_point_t, n - 1);
    const double wrap_gap = (first.x - desc->x.minimum) + (desc->x.maximum - last.x);
    if(!(wrap_gap > COLORZONES_MIN_X_DISTANCE))
    {
      g_hash_table_unref(values);
      dt_remote_error_t *wrap_error = dt_remote_curve_registry_error_new(
        DT_REMOTE_ERR_INVALID_VALUE,
        _("curve '%s' periodic wrap gap is %.17g; minimum is %.17g"),
        desc->name, wrap_gap, (double)COLORZONES_MIN_X_DISTANCE);
      wrap_error->details_json = g_strdup_printf(
        "{\"parameter\":\"%s\",\"constraint\":\"wrap_spacing\"}", desc->name);
      deliver_error(wrap_error, error);
      return FALSE;
    }
  }
  g_hash_table_unref(values);
  return TRUE;
}

static const char *const s_colorzones_prepare_fields[] = { "channel" };

static const dt_remote_curve_module_adapter_t s_colorzones_adapter = {
  .operation = "colorzones",
  .minimum_params_version = 5,
  .maximum_params_version = 5,
  .curves = s_colorzones_curves,
  .curve_count = G_N_ELEMENTS(s_colorzones_curves),
  .prepare_fields = s_colorzones_prepare_fields,
  .prepare_field_count = G_N_ELEMENTS(s_colorzones_prepare_fields),
  .prepare = colorzones_prepare,
  .validate_completed = colorzones_validate_completed,
};
```

Add `&s_colorzones_adapter,` to `s_adapters[]`.

- [ ] **Step 5: Run all curve suites, then the full suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R 'test_remote_curve' --output-on-failure && ctest --test-dir build`
Expected: all PASS, 13/13.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve.c
git commit -m "remote_curve_registry: colorzones semantic curve adapter"
```

---

### Task 4: basecurve adapter

One semantic curve `curve.master` on native channel 0; channels 1/2 are
reserved in the params struct and stay invisible (no descriptors → never
advertised, never written). Facts verified against `src/iop/basecurve.c`:
params v6; `dt_iop_basecurve_node_t basecurve[3][20]` with float x/y;
`basecurve_nodes[3]`; `basecurve_type[3]` (`$DEFAULT: MONOTONE_HERMITE`).
Camera presets ship adjacent gaps ~0.001 (e.g. the Nikon D7200 preset:
0.000618 → 0.001639), so like tonecurve the spacing rule is NONE.

**Files:**
- Modify: `src/control/remote_curve_registry.c`
- Test: `src/tests/unittests/control/test_remote_curve_registry.c`, `src/tests/unittests/control/test_remote_curve.c`

**Interfaces:**
- Consumes: Task 1 helpers, Task 2 macro/fixture.
- Produces: `s_basecurve_adapter` (op `"basecurve"`, versions 6..6), ID `"curve.master"`.

- [ ] **Step 1: Write the failing tests**

In `test_remote_curve_registry.c`:

```c
static void test_basecurve_schema_exposes_single_master_curve(void **state)
{
  (void)state;
  assert_non_null(dt_remote_curve_registry_lookup("basecurve", 6));
  assert_null(dt_remote_curve_registry_lookup("basecurve", 5));

  dt_iop_module_so_t *so = dt_iop_get_module_so("basecurve");
  assert_non_null(so);
  GPtrArray *schemas = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_list_schema(so, &schemas, &error));
  assert_null(error);
  assert_int_equal(schemas->len, 1);
  const dt_remote_curve_schema_t *master = g_ptr_array_index(schemas, 0);
  assert_string_equal(master->name, "curve.master");
  assert_int_equal(master->writability, DT_REMOTE_WRITABLE_NOW);
  assert_null(master->active_when);
  assert_false(master->periodic_x);
  g_ptr_array_unref(schemas);
}
```

In `test_remote_curve.c`:

```c
static void test_basecurve_adapter_master_write_leaves_reserved_channels_untouched(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = adapter_fixture_new("basecurve");

  const dt_remote_curve_point_t s_curve[] = {
    { 0.0, 0.0 }, { 0.25, 0.15 }, { 0.75, 0.85 }, { 1.0, 1.0 }
  };
  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.master", s_curve, 4, TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));

  void *projected = g_malloc0(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *master = rgbcurve_read_value(fixture, projected, "curve.master", &values);
  assert_curve_points(master, s_curve, 4, 1e-6);
  assert_int_equal(master->interpolation, DT_REMOTE_CURVE_MONOTONE_HERMITE);
  g_hash_table_unref(values);

  // reserved native channels 1/2 stay exactly as the pre-patch params had
  // them (compare the raw counts through introspection)
  dt_introspection_field_t *field = NULL;
  int *counts_before = rgbcurve_field_ptr(fixture, fixture->module->params, "basecurve_nodes", &field);
  int *counts_after = rgbcurve_field_ptr(fixture, projected, "basecurve_nodes", &field);
  assert_int_equal(counts_after[1], counts_before[1]);
  assert_int_equal(counts_after[2], counts_before[2]);

  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}
```

Register both.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R 'test_remote_curve' --output-on-failure`
Expected: the two new tests FAIL (registry lookup NULL / unknown semantic curve).

- [ ] **Step 3: Implement the basecurve registry section**

After the colorzones section:

```c
/* ---------------------------------------------------------------------- */
/* basecurve descriptor table (curve-classes design doc SS Initial         */
/* registry mapping / basecurve). Params v6: basecurve[3][20] node         */
/* structs, basecurve_nodes[3], basecurve_type[3]; only channel 0 is       */
/* meaningful -- 1/2 are reserved and stay invisible (no descriptor).      */
/* ---------------------------------------------------------------------- */

ADAPTER_CHANNEL_PATH(s_bc_nodes_ch0, "basecurve", 0)
ADAPTER_CHANNEL_PATH(s_bc_count_ch0, "basecurve_nodes", 0)
ADAPTER_CHANNEL_PATH(s_bc_type_ch0, "basecurve_type", 0)

static const dt_remote_curve_descriptor_t s_basecurve_curves[] = {
  {
    .name = "curve.master",
    .display_name_msgid = N_("base curve"),
    .description_msgid = NULL,
    .native = { .nodes = s_bc_nodes_ch0, .count = s_bc_count_ch0, .type = s_bc_type_ch0,
                .x_field = "x", .y_field = "y",
                .internal_version = s_no_internal_version_path, .internal_version_value = 0 },
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .minimum_points = 2,
    .maximum_points = 20,
    // camera presets ship adjacent gaps ~0.001 (see the task brief); any
    // nonzero floor would reject patches while a factory preset is loaded
    .minimum_x_spacing = 0.0,
    .adjacent_spacing_rule = DT_REMOTE_SPACING_NONE,
    .minimum_wrap_spacing = 0.0,
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
    .strict_x_order = TRUE,
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,
    .active_when = NULL,
    .writable_when = NULL,
    .periodic_when = NULL,
  },
};

static const dt_remote_curve_module_adapter_t s_basecurve_adapter = {
  .operation = "basecurve",
  .minimum_params_version = 6,
  .maximum_params_version = 6,
  .curves = s_basecurve_curves,
  .curve_count = G_N_ELEMENTS(s_basecurve_curves),
  .prepare_fields = NULL,
  .prepare_field_count = 0,
  .prepare = NULL,
  .validate_completed = adapter_validate_active_curves,
};
```

Add `&s_basecurve_adapter,` to `s_adapters[]` (now 4 entries).

- [ ] **Step 4: Run all curve suites, then the full suite**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R 'test_remote_curve' --output-on-failure && ctest --test-dir build`
Expected: all PASS, 13/13.

- [ ] **Step 5: Run the Python unit suite (regression only — no sidecar change)**

Run: `cd tools/mcp && .venv/bin/pytest -q && cd ../..`
Expected: `91 passed` (same as before; nothing sidecar-side depends on which ops carry curves).

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve_registry.c \
        src/tests/unittests/control/test_remote_curve.c
git commit -m "remote_curve_registry: basecurve semantic curve adapter"
```

---

### Task 5: Live integration gates

One new integration module mirroring `tools/mcp/tests/integration/test_curves.py`
(the rgbcurve gates). Reuse its conventions: `harness.connected_client`,
`harness.wait_for_stable_revision`, `harness.undo_latest`, `ProtocolError`,
compare-and-swap with `expected_revision`, and its local `_render` retry
helper (copy it — the integration tests directory is not a package, so
cross-file imports of test helpers are not available).

**Files:**
- Create: `tools/mcp/tests/integration/test_curves_milestone3.py`

**Interfaces:**
- Consumes: wire methods `get_module_schema`/`get_module_params`/`set_module_params`/`undo` exactly as `test_curves.py` does; semantic IDs from Tasks 2–4.

- [ ] **Step 1: Write the integration tests**

Create `tools/mcp/tests/integration/test_curves_milestone3.py`:

```python
"""Milestone-3 live gates: tonecurve, colorzones, and basecurve semantic curves.

Order matters like in test_curves.py: the schema/default assertions run
before any test mutates the module in question. Each test that mutates
restores via undo so the session stays reusable.
"""

import pytest

from . import harness
from darktable_mcp.errors import ProtocolError

pytestmark = pytest.mark.integration


async def _schema_ids(client, module):
    schema = await client.call("get_module_schema", {"module": module})
    return [field["name"] for field in schema.get("semantic_fields", [])]


async def _semantic_values(client, module):
    params = await client.call("get_module_params", {"module": module})
    return params["semantic_values"], params


async def _patch(client, module, values=None, semantic=None, enable=False):
    revision = await harness.wait_for_stable_revision(client)
    request = {"module": module, "values": values or {}, "expected_revision": revision}
    if semantic is not None:
        request["semantic_values"] = semantic
    if enable:
        request["enable"] = True
    return await client.call("set_module_params", request)


def _curve(points, interpolation=None):
    entry = {"class": "curve", "points": [{"x": x, "y": y} for x, y in points]}
    if interpolation is not None:
        entry["interpolation"] = interpolation
    return entry


async def test_tonecurve_schema_and_ab_gating(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "tonecurve") == [
            "curve.lightness",
            "curve.a",
            "curve.b",
        ]
        values, _ = await _semantic_values(client, "tonecurve")
        assert values["curve.lightness"]["active"] is True
        assert values["curve.a"]["active"] is False  # default mode is linked RGB

        # a-channel write while linked must fail atomically
        with pytest.raises(ProtocolError) as excinfo:
            await _patch(client, "tonecurve",
                         semantic={"curve.a": _curve([[0.0, 0.1], [1.0, 0.9]])})
        assert excinfo.value.code == "unsupported_field"

        # one request: switch to independent Lab channels and shape a
        await _patch(
            client,
            "tonecurve",
            values={"tonecurve_autoscale_ab": "DT_S_SCALE_MANUAL"},
            semantic={"curve.a": _curve([[0.0, 0.0], [0.5, 0.4], [1.0, 1.0]],
                                        "catmull_rom")},
            enable=True,
        )
        values, _ = await _semantic_values(client, "tonecurve")
        assert values["curve.a"]["active"] is True
        assert [round(p["y"], 4) for p in values["curve.a"]["points"]] == [0.0, 0.4, 1.0]

        await harness.undo_latest(client)


async def test_colorzones_periodicity_wrap_and_select_by_reset(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "colorzones") == [
            "curve.lightness",
            "curve.chroma",
            "curve.hue",
        ]
        values, _ = await _semantic_values(client, "colorzones")
        # default select-by is hue: periodic, defaults pulled off the edges
        assert values["curve.hue"]["periodic_x"] is True
        assert [round(p["x"], 4) for p in values["curve.hue"]["points"]] == [0.25, 0.75]

        # edge-pinned points are an invalid periodic curve in hue mode
        with pytest.raises(ProtocolError) as excinfo:
            await _patch(client, "colorzones",
                         semantic={"curve.hue": _curve([[0.0, 0.4], [1.0, 0.6]])})
        assert excinfo.value.code == "invalid_value"
        assert excinfo.value.details["constraint"] == "wrap_spacing"

        # a valid periodic patch lands
        await _patch(client, "colorzones",
                     semantic={"curve.hue": _curve([[0.2, 0.45], [0.8, 0.55]])},
                     enable=True)
        values, _ = await _semantic_values(client, "colorzones")
        assert [round(p["y"], 4) for p in values["curve.hue"]["points"]] == [0.45, 0.55]

        # scalar-only select-by change resets every curve to the new axis's
        # defaults, mirroring the GUI
        await _patch(client, "colorzones", values={"channel": "DT_IOP_COLORZONES_L"})
        values, _ = await _semantic_values(client, "colorzones")
        assert values["curve.hue"]["periodic_x"] is False
        assert [round(p["x"], 4) for p in values["curve.hue"]["points"]] == [0.0, 1.0]
        assert [round(p["y"], 4) for p in values["curve.hue"]["points"]] == [0.5, 0.5]

        await harness.undo_latest(client)  # select-by change
        await harness.undo_latest(client)  # hue patch


async def test_basecurve_master_roundtrip_and_undo(darktable_session):
    async with harness.connected_client(darktable_session) as client:
        assert await _schema_ids(client, "basecurve") == ["curve.master"]
        values, _ = await _semantic_values(client, "basecurve")
        prior = values["curve.master"]["points"]

        result = await _patch(
            client,
            "basecurve",
            semantic={"curve.master": _curve(
                [[0.0, 0.0], [0.25, 0.15], [0.75, 0.85], [1.0, 1.0]],
                "monotone_hermite")},
            enable=True,
        )
        echoed = result["semantic_values"]["curve.master"]["points"]
        assert [round(p["x"], 4) for p in echoed] == [0.0, 0.25, 0.75, 1.0]

        await harness.undo_latest(client)
        values, _ = await _semantic_values(client, "basecurve")
        assert values["curve.master"]["points"] == prior
```

Adapt the exact helper call shapes (`ProtocolError.details`, semantic value
member names `active`/`periodic_x`/`points`, the `semantic_values` request
member, undo helper signature) to what `test_curves.py` actually uses if
they differ — that file is the normative reference for the wire idioms, and
these tests must speak the same dialect. Note the wire member is
`semantic_values` (this layer sits below the sidecar's `curves`
translation, same as `test_curves.py`).

- [ ] **Step 2: Run the live suite**

Run: `cd tools/mcp && DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q; cd ../..`
Expected: previous 20 tests + 3 new all PASS (plus the pre-existing OpenCL skip).
If a new test fails, debug with `superpowers:systematic-debugging` — the
most likely real defect this suite can catch is the colorzones wrap-check
caveat from Task 3 (agreed fallback documented there).

- [ ] **Step 3: Commit**

```bash
git add tools/mcp/tests/integration/test_curves_milestone3.py
git commit -m "mcp tests: milestone 3 live curve gates for tonecurve, colorzones, basecurve"
```

---

### Task 6: Documentation and bookkeeping

**Files:**
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md`
- Modify: `docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-reference.md`
- Modify: `tools/mcp/README.md`
- Modify: `tools/mcp/docs/ubuntu-claude-code-setup.md`
- Modify: `tools/mcp/docs/remote-control.md`
- Modify: `docs/superpowers/plans/2026-07-11-darktable-mcp-milestone3-curve-adapters.md` (this file — status header when done)

- [ ] **Step 1: supported-operations tier moves**

In `2026-07-05-darktable-mcp-supported-operations.md`:
- Move `tonecurve` and `colorzones` from Tier 3 to Tier 1; move `basecurve`
  from Tier 2 to Tier 1. New counts: Tier 1 "46 modules", Tier 2
  "17 modules", Tier 3 "8 modules". New Tier 1 rows (match the rgbcurve
  row's voice):
  - `| \`basecurve\` | base curve | yes | **milestone 3, semantic curves**: \`curve.master\` via \`semantic_values\`; fusion scalars writable; reserved native channels stay hidden |`
  - `| \`colorzones\` | color zones | yes | **milestone 3, semantic curves**: \`curve.lightness\`/\`curve.chroma\`/\`curve.hue\`; periodic when select-by is hue; select-by change resets curves like the GUI |`
  - `| \`tonecurve\` | tone curve | yes | **milestone 3, semantic curves**: \`curve.lightness\` always, \`curve.a\`/\`curve.b\` in independent-Lab mode; color-space scalar writable |`
- Update the "How support is determined" semantic-class sentence: "`rgbcurve`
  is the first" → "`rgbcurve` (milestone 2) then `tonecurve`, `colorzones`,
  and `basecurve` (milestone 3)".
- In the Tier 3 intro, replace the future-candidates sentence: the remaining
  curve-shaped entries are `atrous` (fixed-band parallel arrays — needs a
  new native layout kind) and `rgblevels` (levels triples — needs a levels
  class), both explicitly out of the curve-adapter pattern's reach.

- [ ] **Step 2: protocol reference touch-up**

In `2026-07-05-darktable-mcp-protocol-reference.md`, where `semantic_fields`
/ `semantic_values` name rgbcurve as the curve-bearing op, generalize to
"curve-bearing ops (`rgbcurve`, `tonecurve`, `colorzones`, `basecurve`)".
No wire-shape change: capabilities, members, and error contracts are
untouched. Add one line to the Maintenance section: new curve ops are
registry entries in `src/control/remote_curve_registry.c` and require no
protocol change.

- [ ] **Step 3: sidecar docs**

- `tools/mcp/README.md`: in the `curves` paragraph, replace the
  rgbcurve-only phrasing with the four-module list and note that
  `get_module_schema` is the authoritative source of each module's curve
  IDs.
- `tools/mcp/docs/ubuntu-claude-code-setup.md` §10: after the rgbcurve
  example, add one sentence: the same `curves` argument drives `tonecurve`
  (`curve.lightness`, plus `curve.a`/`curve.b` in independent-Lab mode),
  `colorzones` (`curve.lightness`/`curve.chroma`/`curve.hue`, periodic when
  selecting by hue), and `basecurve` (`curve.master`).
- `tools/mcp/docs/remote-control.md` §6, the capability bullet: append that
  milestone 3 extended the same `curve_params` capability to tonecurve,
  colorzones, and basecurve with no version bump.

- [ ] **Step 4: Final full verification**

Run all three suites:
```bash
ctest --test-dir build
cd tools/mcp && .venv/bin/pytest -q
DARKTABLE_BIN=$PWD/../../build/bin/darktable .venv/bin/pytest -m integration -q
cd ../..
```
Expected: 13/13 C suites; 91 unit; 23 integration passed + 1 OpenCL skip.

- [ ] **Step 5: Mark this plan complete and commit**

Add a `**Status: Complete (<date>).**` header under this plan's title
(matching the milestone-2 plan's convention), tick all checkboxes, then:

```bash
git add docs/ tools/mcp/README.md tools/mcp/docs/
git commit -m "darktable-mcp: milestone 3 docs for tonecurve/colorzones/basecurve curves"
```

---

## Self-review notes (already applied)

- **Spec coverage:** every row of the design doc's "Initial registry
  mapping" for the three modules maps to a task; the two ops the design
  never mapped (`atrous`, `rgblevels`) are explicitly out of scope with the
  layout rationale.
- **Type consistency:** `adapter_*` helper names introduced in Task 1 are
  what Tasks 2–4 call; `ADAPTER_CHANNEL_PATH` and `adapter_fixture_new` are
  introduced in Task 2 and consumed by Tasks 3–4;
  `RGBCURVE_INTERPOLATION_MASK` is reused deliberately (all four modules
  share curve_tools.h's three interpolators).
- **Known judgment calls, verified against source this session:** spacing
  NONE for tonecurve/basecurve (camera presets violate any nonzero floor);
  colorzones wrap check in the adapter, not the descriptor (edge-pinned
  non-hue defaults); colorzones prepare resets curves on select-by change
  (mirrors `gui_changed`); `splines_version` stamped to V2 on curve writes
  (design-doc decision — note it changes spline interpretation of a legacy
  V1 edit's other curves the first time MCP touches any of them).
