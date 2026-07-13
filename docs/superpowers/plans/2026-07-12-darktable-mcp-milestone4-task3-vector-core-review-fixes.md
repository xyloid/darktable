# darktable MCP Milestone 4 Task 3 Vector Core Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every finding in the approved Milestone 4 Task 3 vector-core review before Task 4 generalizes the remote-edit and protocol seams.

**Architecture:** Keep the existing split: `remote_vector.c` owns pure value validation and mutation, while `remote_vector_registry.c` owns compiled adapter validation, caching, schema conversion, and reads. Harden each boundary in place, separate immutable intrinsic validation from mutable cross-class lookup state, and add regression-sensitive tests through the existing test-local lookup seam and real `borders` module introspection.

**Tech Stack:** C99, GLib (`GArray`, `GPtrArray`, `GHashTable`), darktable introspection, cmocka, CMake/CTest.

## Global Constraints

- Scope is limited to findings `VEC3-001` through `VEC3-010` in `docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review.md`.
- Do not add a wire member, capability, shipped vector adapter, or MCP-side change; those remain Milestone 4 Tasks 4–9.
- Registry or introspection drift returns `DT_REMOTE_ERR_INTERNAL` and never returns a partial schema/value result or modifies params.
- Caller values are never clamped, sorted, deduplicated, replaced, or otherwise repaired.
- Vector components remain doubles through validation and narrow to float only at the native write.
- Only `[0, component_count)` may be written; `[component_count, native_capacity)` and every unrelated params byte remain bit-identical.
- Operations with no vector adapter retain silent schema/read degradation; vector writes to them retain `DT_REMOTE_ERR_UNKNOWN_FIELD`.
- Lookup overrides remain process-wide, single-threaded test seams and must be reset after each test.
- Every cached-adapter test uses a distinct adapter address unless it explicitly tests repeat validation of that address.
- Complete each task's focused test and commit before starting the next task.

## Finding-to-task map

| Finding | Plan task |
|---|---|
| VEC3-001 | Task 2 |
| VEC3-002 | Task 1 |
| VEC3-003 | Task 4 |
| VEC3-004 | Task 3 |
| VEC3-005 | Task 5 |
| VEC3-006 | Task 7 |
| VEC3-007 | Task 3 |
| VEC3-008 | Task 8 |
| VEC3-009 | Task 8 |
| VEC3-010 | Tasks 6–7 |

## File responsibility map

- `src/control/remote_vector.c`: reject non-finite values before range/order checks; leave mutation ordering unchanged.
- `src/control/remote_vector_registry.c`: validate adapter/descriptor/subtype/native metadata and separate intrinsic cached results from cross-class collision state.
- `src/control/remote_vector.h`: synchronize the public internal contracts with hardened behavior and error details.
- `src/tests/unittests/control/test_remote_vector.c`: add all closure tests, private fixture helpers, and durable no-adapter overrides.
- `docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review.md`: record closure only after tests and a fresh review pass.

---

### Task 1: Reject non-finite vector components (`VEC3-002`)

**Files:**
- Modify: `src/control/remote_vector.c:24-25,120-188`
- Modify: `src/control/remote_vector.h:231-257`
- Test: `src/tests/unittests/control/test_remote_vector.c:228-412,1485-1495`

**Interfaces:**
- Consumes: `gboolean dt_remote_vector_validate(const dt_remote_vector_descriptor_t *, const GArray *, dt_remote_error_t **)`.
- Produces: the same signature, with non-finite elements rejected using constraint `"non_finite"` before domain or LEVELS checks.

- [ ] **Step 1: Add pure-validator regression tests**

Add these tests beside the existing domain-boundary tests:

```c
static void test_vector_validate_rejects_non_finite_components(void **state)
{
  (void)state;
  const double invalid[] = { NAN, INFINITY, -INFINITY };

  for(guint i = 0; i < G_N_ELEMENTS(invalid); i++)
  {
    dt_remote_vector_descriptor_t desc = make_plain_descriptor();
    const double raw[] = { 0.5, invalid[i], 0.0 };
    GArray *values = make_values(raw, G_N_ELEMENTS(raw));
    GArray *before = make_values(raw, G_N_ELEMENTS(raw));
    dt_remote_error_t *error = NULL;

    assert_false(dt_remote_vector_validate(&desc, values, &error));
    assert_vector_error_details(error, "vector.plain", 1, "non_finite");
    assert_memory_equal(values->data, before->data, values->len * sizeof(double));

    dt_remote_error_free(error);
    g_array_unref(before);
    g_array_unref(values);
  }
}

static void test_vector_validate_levels_rejects_nan_in_every_position(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor((double)FLT_EPSILON);

  for(guint bad_index = 0; bad_index < desc.component_count; bad_index++)
  {
    double raw[] = { 0.1, 0.5, 0.9 };
    raw[bad_index] = NAN;
    GArray *values = make_values(raw, G_N_ELEMENTS(raw));
    dt_remote_error_t *error = NULL;

    assert_false(dt_remote_vector_validate(&desc, values, &error));
    assert_vector_error_details(error, "vector.levels", (int)bad_index, "non_finite");

    dt_remote_error_free(error);
    g_array_unref(values);
  }
}

static void test_vector_validate_accepts_exact_finite_minima(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  const double raw[] = { 0.0, 0.0, -1.0 };
  GArray *values = make_values(raw, G_N_ELEMENTS(raw));
  dt_remote_error_t *error = NULL;

  assert_true(dt_remote_vector_validate(&desc, values, &error));
  assert_null(error);
  g_array_unref(values);
}
```

Register all three immediately after the existing exact-maximum validator test:

```c
cmocka_unit_test(test_vector_validate_rejects_non_finite_components),
cmocka_unit_test(test_vector_validate_levels_rejects_nan_in_every_position),
cmocka_unit_test(test_vector_validate_accepts_exact_finite_minima),
```

- [ ] **Step 2: Run the focused test to verify the new non-finite cases fail**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: the PLAIN or LEVELS non-finite test fails because `dt_remote_vector_validate()` returns TRUE for NaN. The exact-minimum test passes.

- [ ] **Step 3: Reject non-finite values before range checks**

Add `<math.h>` to `remote_vector.c`, then place this branch immediately after reading `value` and `component`:

```c
if(!isfinite(value))
{
  if(error)
    *error = dt_remote_vector_validate_error_new(
      parameter, (int)i, "non_finite",
      _("vector '%s' component %u has a non-finite value"), parameter, i);
  return FALSE;
}
```

Keep the existing inclusive range check directly after it. Do not cast to float in the validator.

- [ ] **Step 4: Update the validator contract**

In `remote_vector.h`, insert:

```c
 *   - every component must be finite ("non_finite");
 *   - each finite component must fall within its own [minimum, maximum]
 *     domain, inclusive ("domain");
```

- [ ] **Step 5: Run focused and curve-regression tests**

```bash
cmake --build build --target test_remote_vector test_remote_curve -j2
ctest --test-dir build -R '^test_remote_(vector|curve)$' --output-on-failure
```

Expected: both tests pass; NaN reports `non_finite`, while finite range and epsilon-gap behavior is unchanged.

- [ ] **Step 6: Commit**

```bash
git add src/control/remote_vector.c \
        src/control/remote_vector.h \
        src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector: reject non-finite vector components"
```

---

### Task 2: Validate adapter and component metadata (`VEC3-001`)

**Files:**
- Modify: `src/control/remote_vector_registry.c:45-46,179-339`
- Modify: `src/control/remote_vector.h:148-166`
- Test: `src/tests/unittests/control/test_remote_vector.c:422-478,540-803,1475-1545`

**Interfaces:**
- Consumes: `dt_remote_vector_registry_validate(adapter, so, error)` and the existing real-`borders` fixture.
- Produces: pre-cache adapter-envelope validation and per-descriptor component metadata validation; invalid compiled metadata returns `DT_REMOTE_ERR_INTERNAL` without dereferencing invalid pointers.

- [ ] **Step 1: Add allocation helpers for unique cached-adapter identities**

Add `s_registry_test_allocations` near the harness globals. Place
`new_test_adapter()` and `assert_vector_registry_rejects()` immediately after
`make_color_descriptor()` so the factory has a visible declaration:

```c
static GPtrArray *s_registry_test_allocations = NULL;

static dt_remote_vector_module_adapter_t *new_test_adapter(
  dt_remote_vector_descriptor_t **out_descriptor)
{
  dt_remote_vector_descriptor_t *descriptor = g_new(dt_remote_vector_descriptor_t, 1);
  *descriptor = make_color_descriptor();
  dt_remote_vector_module_adapter_t *adapter = g_new0(dt_remote_vector_module_adapter_t, 1);
  *adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders",
    .minimum_params_version = 4,
    .maximum_params_version = 4,
    .vectors = descriptor,
    .vector_count = 1,
  };
  g_ptr_array_add(s_registry_test_allocations, descriptor);
  g_ptr_array_add(s_registry_test_allocations, adapter);
  *out_descriptor = descriptor;
  return adapter;
}

static void assert_vector_registry_rejects(
  const dt_remote_vector_module_adapter_t *adapter,
  const dt_iop_module_so_t *so)
{
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}
```

Initialize and release the allocation array in the group fixture:

```c
s_registry_test_allocations = g_ptr_array_new_with_free_func(g_free);
```

```c
g_clear_pointer(&s_registry_test_allocations, g_ptr_array_unref);
```

- [ ] **Step 2: Add failing adapter-envelope tests**

```c
static void test_registry_validate_rejects_invalid_adapter_envelope(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *null_vectors = new_test_adapter(&descriptor);
  null_vectors->vectors = NULL;
  assert_vector_registry_rejects(null_vectors, so);

  dt_remote_vector_module_adapter_t *null_prepare = new_test_adapter(&descriptor);
  null_prepare->prepare_field_count = 1;
  null_prepare->prepare_fields = NULL;
  assert_vector_registry_rejects(null_prepare, so);

  static const char *const empty_prepare[] = { "" };
  dt_remote_vector_module_adapter_t *empty_prepare_name = new_test_adapter(&descriptor);
  empty_prepare_name->prepare_field_count = 1;
  empty_prepare_name->prepare_fields = empty_prepare;
  assert_vector_registry_rejects(empty_prepare_name, so);

  dt_remote_vector_module_adapter_t *wrong_operation = new_test_adapter(&descriptor);
  wrong_operation->operation = "watermark";
  assert_vector_registry_rejects(wrong_operation, so);

  dt_remote_vector_module_adapter_t *null_operation = new_test_adapter(&descriptor);
  null_operation->operation = NULL;
  assert_vector_registry_rejects(null_operation, so);

  dt_remote_vector_module_adapter_t *empty_operation = new_test_adapter(&descriptor);
  empty_operation->operation = "";
  assert_vector_registry_rejects(empty_operation, so);

  dt_remote_vector_module_adapter_t *reversed_versions = new_test_adapter(&descriptor);
  reversed_versions->minimum_params_version = 5;
  reversed_versions->maximum_params_version = 4;
  assert_vector_registry_rejects(reversed_versions, so);

  dt_remote_vector_module_adapter_t *outside_version = new_test_adapter(&descriptor);
  outside_version->minimum_params_version = 1;
  outside_version->maximum_params_version = 3;
  assert_vector_registry_rejects(outside_version, so);
}
```

- [ ] **Step 3: Add failing descriptor/component metadata tests**

```c
static void test_registry_validate_rejects_invalid_descriptor_metadata(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_remote_vector_descriptor_t *descriptor = NULL;

  dt_remote_vector_module_adapter_t *null_name = new_test_adapter(&descriptor);
  descriptor->name = NULL;
  assert_vector_registry_rejects(null_name, so);

  dt_remote_vector_module_adapter_t *empty_name = new_test_adapter(&descriptor);
  descriptor->name = "";
  assert_vector_registry_rejects(empty_name, so);

  dt_remote_vector_module_adapter_t *zero_components = new_test_adapter(&descriptor);
  descriptor->component_count = 0;
  assert_vector_registry_rejects(zero_components, so);

  dt_remote_vector_module_adapter_t *null_components = new_test_adapter(&descriptor);
  descriptor->components = NULL;
  assert_vector_registry_rejects(null_components, so);

  static const dt_remote_vector_component_t null_component_name[] = {
    { NULL, 0.0, 1.0 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_component_name = new_test_adapter(&descriptor);
  descriptor->components = null_component_name;
  assert_vector_registry_rejects(bad_component_name, so);

  static const dt_remote_vector_component_t empty_component_name[] = {
    { "", 0.0, 1.0 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_empty_component_name = new_test_adapter(&descriptor);
  descriptor->components = empty_component_name;
  assert_vector_registry_rejects(bad_empty_component_name, so);

  static const dt_remote_vector_component_t reversed_bounds[] = {
    { "red", 1.0, 0.0 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_bounds = new_test_adapter(&descriptor);
  descriptor->components = reversed_bounds;
  assert_vector_registry_rejects(bad_bounds, so);

  static const dt_remote_vector_component_t nan_bounds[] = {
    { "red", NAN, 1.0 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_nan_bounds = new_test_adapter(&descriptor);
  descriptor->components = nan_bounds;
  assert_vector_registry_rejects(bad_nan_bounds, so);

  static const dt_remote_vector_component_t infinite_bounds[] = {
    { "red", 0.0, INFINITY }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_infinite_bounds = new_test_adapter(&descriptor);
  descriptor->components = infinite_bounds;
  assert_vector_registry_rejects(bad_infinite_bounds, so);
}
```

Register both tests with the registry-validation group:

```c
cmocka_unit_test(test_registry_validate_rejects_invalid_adapter_envelope),
cmocka_unit_test(test_registry_validate_rejects_invalid_descriptor_metadata),
```

- [ ] **Step 4: Run the focused test and capture the fail-closed failures**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: the null-vector case crashes or fails before producing a controlled error, and valid-path NULL-components/invalid-bounds cases are accepted.

- [ ] **Step 5: Implement adapter and descriptor metadata validation**

Add `<math.h>` and these helpers before `vector_descriptor_is_valid()`:

```c
static gboolean nonempty_string(const char *value)
{
  return value && value[0] != '\0';
}

static gboolean vector_adapter_envelope_is_valid(
  const dt_remote_vector_module_adapter_t *adapter,
  const dt_iop_module_so_t *so,
  const dt_introspection_t *intro)
{
  if(!nonempty_string(adapter->operation)
     || g_strcmp0(adapter->operation, so->op)
     || adapter->minimum_params_version > adapter->maximum_params_version
     || intro->params_version < 0
     || (guint)intro->params_version < adapter->minimum_params_version
     || (guint)intro->params_version > adapter->maximum_params_version
     || (adapter->vector_count > 0 && !adapter->vectors)
     || (adapter->prepare_field_count > 0 && !adapter->prepare_fields))
    return FALSE;

  for(guint i = 0; i < adapter->prepare_field_count; i++)
    if(!nonempty_string(adapter->prepare_fields[i])) return FALSE;
  return TRUE;
}

static gboolean vector_component_metadata_is_valid(
  const dt_remote_vector_descriptor_t *desc)
{
  if(!nonempty_string(desc->name) || desc->component_count == 0 || !desc->components)
    return FALSE;

  for(guint i = 0; i < desc->component_count; i++)
  {
    const dt_remote_vector_component_t *component = &desc->components[i];
    if(!nonempty_string(component->name)
       || !isfinite(component->minimum)
       || !isfinite(component->maximum)
       || component->minimum > component->maximum)
      return FALSE;
  }
  return TRUE;
}
```

After introspection is loaded and before `lookup_cache_entry()`, reject an invalid adapter envelope with a fresh `DT_REMOTE_ERR_INTERNAL`. At the beginning of `vector_descriptor_is_valid()`, add:

```c
if(!vector_component_metadata_is_valid(desc)) return FALSE;
```

Do not call duplicate-name, collision, or descriptor loops when the adapter envelope is invalid.

- [ ] **Step 6: Update the registry contract in `remote_vector.h`**

Document operation/version matching, pointer/count pairs, nonempty IDs, and finite ordered component bounds as fail-closed registry checks. Retain the native-path, duplicate-ID, cross-class, COLOR, and LEVELS requirements.

- [ ] **Step 7: Run focused tests**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: all vector tests pass and malformed envelopes return `DT_REMOTE_ERR_INTERNAL` instead of crashing.

- [ ] **Step 8: Commit**

```bash
git add src/control/remote_vector_registry.c \
        src/control/remote_vector.h \
        src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector_registry: validate adapter and component metadata"
```

---

### Task 3: Enforce subtype and native-array invariants (`VEC3-004`, `VEC3-007`)

**Files:**
- Modify: `src/control/remote_vector_registry.c:200-227`
- Modify: `src/control/remote_vector.h:148-166`
- Test: `src/tests/unittests/control/test_remote_vector.c:511-803,1475-1545`

**Interfaces:**
- Consumes: Task 2's `nonempty_string()`, `vector_component_metadata_is_valid()`, and unique-adapter test factory.
- Produces: exhaustive `dt_remote_vector_subtype_t` validation and proof that a resolved native leaf is a usable `float` array with a non-NULL float element descriptor.

- [ ] **Step 1: Add native-path constants and a synthetic-introspection helper**

```c
static const dt_remote_path_segment_t s_size_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "size" },
};
static const dt_remote_introspection_path_t s_size_path = {
  .segments = s_size_segments, .length = G_N_ELEMENTS(s_size_segments),
};
static const dt_remote_path_segment_t s_aspect_text_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "aspect_text" },
};
static const dt_remote_introspection_path_t s_aspect_text_path = {
  .segments = s_aspect_text_segments, .length = G_N_ELEMENTS(s_aspect_text_segments),
};
static const dt_remote_path_segment_t s_missing_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "missing_vector_field" },
};
static const dt_remote_introspection_path_t s_missing_path = {
  .segments = s_missing_segments, .length = G_N_ELEMENTS(s_missing_segments),
};

static dt_introspection_t *s_private_vector_intro = NULL;

static dt_introspection_t *private_vector_get_introspection(void)
{
  return s_private_vector_intro;
}

static void init_private_vector_so(dt_iop_module_so_t *out,
                                   const char *operation,
                                   dt_introspection_t *intro)
{
  memset(out, 0, sizeof(*out));
  g_strlcpy(out->op, operation, sizeof(out->op));
  out->get_introspection = private_vector_get_introspection;
  s_private_vector_intro = intro;
}
```

- [ ] **Step 2: Add failing subtype tests**

```c
static void test_registry_validate_rejects_invalid_subtype_metadata(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_remote_vector_descriptor_t *descriptor = NULL;

  dt_remote_vector_module_adapter_t *unknown = new_test_adapter(&descriptor);
  descriptor->subtype = (dt_remote_vector_subtype_t)99;
  assert_vector_registry_rejects(unknown, so);

  dt_remote_vector_module_adapter_t *plain_color_space = new_test_adapter(&descriptor);
  descriptor->color_space = "display_rgb";
  assert_vector_registry_rejects(plain_color_space, so);

  dt_remote_vector_module_adapter_t *plain_ordering = new_test_adapter(&descriptor);
  descriptor->strictly_increasing = TRUE;
  assert_vector_registry_rejects(plain_ordering, so);

  dt_remote_vector_module_adapter_t *plain_gap = new_test_adapter(&descriptor);
  descriptor->minimum_gap = FLT_EPSILON;
  assert_vector_registry_rejects(plain_gap, so);

  dt_remote_vector_module_adapter_t *empty_color_space = new_test_adapter(&descriptor);
  descriptor->subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor->color_space = "";
  assert_vector_registry_rejects(empty_color_space, so);

  dt_remote_vector_module_adapter_t *color_ordering = new_test_adapter(&descriptor);
  descriptor->subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor->color_space = "display_rgb";
  descriptor->strictly_increasing = TRUE;
  assert_vector_registry_rejects(color_ordering, so);

  dt_remote_vector_module_adapter_t *levels_color_space = new_test_adapter(&descriptor);
  descriptor->subtype = DT_REMOTE_VECTOR_LEVELS;
  descriptor->strictly_increasing = TRUE;
  descriptor->color_space = "display_rgb";
  assert_vector_registry_rejects(levels_color_space, so);

  const double bad_gaps[] = { -FLT_EPSILON, NAN, INFINITY };
  for(guint i = 0; i < G_N_ELEMENTS(bad_gaps); i++)
  {
    dt_remote_vector_module_adapter_t *levels_gap = new_test_adapter(&descriptor);
    descriptor->subtype = DT_REMOTE_VECTOR_LEVELS;
    descriptor->strictly_increasing = TRUE;
    descriptor->minimum_gap = bad_gaps[i];
    assert_vector_registry_rejects(levels_gap, so);
  }
}

static void test_registry_validate_accepts_valid_color_and_levels_metadata(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_error_t *error = NULL;

  dt_remote_vector_module_adapter_t *color = new_test_adapter(&descriptor);
  descriptor->subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor->color_space = "display_rgb";
  assert_true(dt_remote_vector_registry_validate(color, so, &error));
  assert_null(error);

  const double valid_gaps[] = { 0.0, FLT_EPSILON };
  for(guint i = 0; i < G_N_ELEMENTS(valid_gaps); i++)
  {
    dt_remote_vector_module_adapter_t *levels = new_test_adapter(&descriptor);
    descriptor->subtype = DT_REMOTE_VECTOR_LEVELS;
    descriptor->strictly_increasing = TRUE;
    descriptor->minimum_gap = valid_gaps[i];
    assert_true(dt_remote_vector_registry_validate(levels, so, &error));
    assert_null(error);
  }
}
```

- [ ] **Step 3: Add isolated native-layout tests**

```c
static const dt_remote_vector_component_t s_rgba_components[] = {
  { "red", 0.0, 1.0 }, { "green", 0.0, 1.0 },
  { "blue", 0.0, 1.0 }, { "alpha", 0.0, 1.0 },
};

static void test_registry_validate_isolates_native_layout_failures(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_remote_vector_descriptor_t *descriptor = NULL;

  dt_remote_vector_module_adapter_t *missing = new_test_adapter(&descriptor);
  descriptor->native = s_missing_path;
  assert_vector_registry_rejects(missing, so);

  dt_remote_vector_module_adapter_t *scalar = new_test_adapter(&descriptor);
  descriptor->native = s_size_path;
  descriptor->component_count = 1;
  descriptor->components = s_rgb_components;
  descriptor->native_capacity = 1;
  assert_vector_registry_rejects(scalar, so);

  dt_remote_vector_module_adapter_t *char_array = new_test_adapter(&descriptor);
  descriptor->native = s_aspect_text_path;
  descriptor->native_capacity = 20;
  assert_vector_registry_rejects(char_array, so);

  dt_remote_vector_module_adapter_t *count_overflow = new_test_adapter(&descriptor);
  descriptor->component_count = 4;
  descriptor->components = s_rgba_components;
  descriptor->native_capacity = 3;
  assert_vector_registry_rejects(count_overflow, so);

  dt_remote_vector_module_adapter_t *capacity_mismatch = new_test_adapter(&descriptor);
  descriptor->component_count = 2;
  descriptor->components = s_rg_components;
  descriptor->native_capacity = 4;
  assert_vector_registry_rejects(capacity_mismatch, so);
}
```

For the missing float element descriptor, wrap this case in
`test_registry_validate_rejects_missing_float_element_descriptor()` so all
shallow-copy lifetimes enclose the validation call:

```c
static void test_registry_validate_rejects_missing_float_element_descriptor(void **state)
{
  (void)state;
dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
dt_introspection_t broken_intro = *so->get_introspection();
dt_introspection_field_t broken_root = *broken_intro.field;
dt_introspection_field_t broken_color = { 0 };
dt_introspection_field_t **fields =
  g_new(dt_introspection_field_t *, broken_root.Struct.entries + 1);
for(guint i = 0; i <= broken_root.Struct.entries; i++)
{
  fields[i] = broken_root.Struct.fields[i];
  if(fields[i] && !g_strcmp0(fields[i]->header.field_name, "color"))
  {
    broken_color = *fields[i];
    broken_color.Array.field = NULL;
    fields[i] = &broken_color;
  }
}
broken_root.Struct.fields = fields;
broken_intro.field = &broken_root;
dt_iop_module_so_t private_so;
init_private_vector_so(&private_so, "borders", &broken_intro);
dt_remote_vector_descriptor_t *descriptor = NULL;
dt_remote_vector_module_adapter_t *missing_element = new_test_adapter(&descriptor);
assert_vector_registry_rejects(missing_element, &private_so);
s_private_vector_intro = NULL;
g_free(fields);
}
```

Register the subtype and native-layout tests:

```c
cmocka_unit_test(test_registry_validate_rejects_invalid_subtype_metadata),
cmocka_unit_test(test_registry_validate_accepts_valid_color_and_levels_metadata),
cmocka_unit_test(test_registry_validate_isolates_native_layout_failures),
cmocka_unit_test(test_registry_validate_rejects_missing_float_element_descriptor),
```

Also reset `s_private_vector_intro = NULL` in lookup-test teardown.

- [ ] **Step 4: Run tests and identify the new failures**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: missing/scalar/non-float/count/capacity tests already pass; unknown/extraneous subtype metadata, invalid gaps, and a missing element descriptor fail because they are accepted.

- [ ] **Step 5: Implement exhaustive subtype and element validation**

```c
static gboolean vector_subtype_metadata_is_valid(
  const dt_remote_vector_descriptor_t *desc)
{
  switch(desc->subtype)
  {
    case DT_REMOTE_VECTOR_PLAIN:
      return !desc->color_space && !desc->strictly_increasing && desc->minimum_gap == 0.0;
    case DT_REMOTE_VECTOR_COLOR:
      return nonempty_string(desc->color_space)
             && !desc->strictly_increasing && desc->minimum_gap == 0.0;
    case DT_REMOTE_VECTOR_LEVELS:
      return !desc->color_space && desc->strictly_increasing
             && isfinite(desc->minimum_gap) && desc->minimum_gap >= 0.0;
    default:
      return FALSE;
  }
}
```

Call it after component metadata validation. Replace the native type check with:

```c
if(!array_field
   || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
   || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT
   || !array_field->Array.field
   || array_field->Array.field->header.type != DT_INTROSPECTION_TYPE_FLOAT
   || array_field->Array.field->header.size != sizeof(float))
  return FALSE;
```

Remove the old two one-way subtype checks.

- [ ] **Step 6: Update the registry contract and run tests**

Document the exhaustive subtype rules and usable float element descriptor in `remote_vector.h`, then run:

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: all tests pass and every native-layout failure has an isolated test.

- [ ] **Step 7: Commit**

```bash
git add src/control/remote_vector_registry.c \
        src/control/remote_vector.h \
        src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector_registry: enforce subtype and native array invariants"
```

---

### Task 4: Separate intrinsic cache state from cross-class uniqueness (`VEC3-003`)

**Files:**
- Modify: `src/control/remote_vector_registry.c:121-177,229-339`
- Modify: `src/control/remote_vector.h:148-166`
- Test: `src/tests/unittests/control/test_remote_vector.c:596-736,1475-1545`

**Interfaces:**
- Consumes: the curve lookup override and Task 2's safe envelope checks.
- Produces: cache entries representing only immutable vector adapter/introspection validation; curve/vector ID collisions are checked on every validation call.

- [ ] **Step 1: Add override-order regression tests**

```c
static void test_registry_validate_rechecks_curve_collision_after_cached_success(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *adapter = new_test_adapter(&descriptor);
  descriptor->name = "vector.color";
  dt_remote_error_t *error = NULL;

  assert_true(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_null(error);
  dt_remote_curve_registry_set_lookup_override(colliding_curve_lookup_override);
  assert_vector_registry_rejects(adapter, so);
  dt_remote_curve_registry_set_lookup_override(NULL);
  assert_true(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_null(error);
}

static void test_registry_validate_does_not_cache_curve_collision(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *adapter = new_test_adapter(&descriptor);
  descriptor->name = "vector.color";

  dt_remote_curve_registry_set_lookup_override(colliding_curve_lookup_override);
  assert_vector_registry_rejects(adapter, so);
  dt_remote_curve_registry_set_lookup_override(NULL);

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_null(error);
}

static void test_registry_validate_retains_intrinsic_cached_result(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *adapter = new_test_adapter(&descriptor);
  dt_remote_error_t *error = NULL;

  assert_true(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_null(error);
  descriptor->native = s_missing_path;
  assert_true(dt_remote_vector_registry_validate(adapter, so, &error));
  assert_null(error);
}
```

Register all three with lookup setup/teardown:

```c
cmocka_unit_test_setup_teardown(
  test_registry_validate_rechecks_curve_collision_after_cached_success,
  lookup_override_test_setup, lookup_override_test_teardown),
cmocka_unit_test_setup_teardown(
  test_registry_validate_does_not_cache_curve_collision,
  lookup_override_test_setup, lookup_override_test_teardown),
cmocka_unit_test_setup_teardown(
  test_registry_validate_retains_intrinsic_cached_result,
  lookup_override_test_setup, lookup_override_test_teardown),
```

- [ ] **Step 2: Run the focused test to reproduce stale collision state**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: cached-success and cached-collision tests fail; intrinsic-cache retention passes.

- [ ] **Step 3: Refactor validation cache control flow**

Keep adapter-envelope checks outside the cache. Cache duplicate-name, descriptor metadata, subtype/native layout, and predicate results only. Then run collision checking after a successful intrinsic result on both cache hits and misses:

```c
dt_remote_vector_registry_cache_entry_t *cache =
  lookup_cache_entry(adapter, intro->params_version);
if(!cache)
{
  gboolean intrinsic_valid = TRUE;
  const char *first_failure = NULL;
  if(vector_adapter_has_duplicate_names(adapter, &first_failure)) intrinsic_valid = FALSE;
  guint8 dummy_byte = 0;
  for(guint i = 0; i < adapter->vector_count; i++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[i];
    if(!vector_descriptor_is_valid(desc, intro->field, &dummy_byte))
    {
      intrinsic_valid = FALSE;
      if(!first_failure) first_failure = desc->name;
    }
  }
  cache_validation_result(adapter, intro->params_version, intrinsic_valid);
  cache = lookup_cache_entry(adapter, intro->params_version);
}

if(!cache->adapter_valid)
{
  deliver_error(dt_remote_vector_registry_error_new(
                  DT_REMOTE_ERR_INTERNAL,
                  _("vector adapter '%s' does not match introspection version %d"),
                  adapter->operation, intro->params_version),
                error);
  return FALSE;
}

const char *collision_name = NULL;
if(vector_names_collide_with_curve(adapter, (guint)intro->params_version, &collision_name))
{
  deliver_error(dt_remote_vector_registry_error_new(
                  DT_REMOTE_ERR_INTERNAL,
                  _("semantic vector '%s' collides with a curve ID"),
                  collision_name ? collision_name : ""),
                error);
  return FALSE;
}
return TRUE;
```

- [ ] **Step 4: Update cache documentation and run regressions**

State in `remote_vector.h` that intrinsic validation is cached while cross-class uniqueness is evaluated on every call. Run:

```bash
cmake --build build --target test_remote_vector test_remote_curve_registry -j2
ctest --test-dir build -R '^test_remote_(vector|curve_registry)$' --output-on-failure
```

Expected: both tests pass and collisions follow current override state.

- [ ] **Step 5: Commit**

```bash
git add src/control/remote_vector_registry.c \
        src/control/remote_vector.h \
        src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector_registry: keep cross-class collisions out of cache"
```

---

### Task 5: Distinguish active and writable predicates (`VEC3-005`)

**Files:**
- Test: `src/tests/unittests/control/test_remote_vector.c:921-973,1293-1347,1475-1545`

**Interfaces:**
- Consumes: `active_when`, `writable_when`, `dt_remote_vector_read_values()`, scalar projection, and lookup override behavior.
- Produces: regression-sensitive proof that read and apply evaluate the two predicate pointers independently.

- [ ] **Step 1: Add a distinct-predicate read matrix**

```c
static void test_read_values_evaluates_active_and_writable_predicates_independently(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  static const dt_remote_parameter_predicate_t active_portrait = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT",
  };
  static const dt_remote_parameter_predicate_t writable_not_landscape = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_NE,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE",
  };
  static const dt_remote_parameter_predicate_t active_not_auto = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_NE,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO",
  };
  static const dt_remote_parameter_predicate_t writable_auto = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO",
  };
  static dt_remote_vector_descriptor_t descriptors[2];
  descriptors[0] = make_color_descriptor();
  descriptors[0].name = "vector.first";
  descriptors[0].active_when = &active_portrait;
  descriptors[0].writable_when = &writable_not_landscape;
  descriptors[1] = make_color_descriptor();
  descriptors[1].name = "vector.second";
  descriptors[1].native = s_frame_color_path;
  descriptors[1].active_when = &active_not_auto;
  descriptors[1].writable_when = &writable_auto;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = descriptors, .vector_count = 2,
  };
  install_vector_adapter(&adapter);

  dt_introspection_field_t *orient_field = NULL;
  int *orient = dt_introspection_get_child(
    fixture->module->so->get_introspection()->field,
    fixture->module->params, "aspect_orient", &orient_field);
  int auto_value = 0, portrait_value = 0, landscape_value = 0;
  assert_true(dt_introspection_get_enum_value(orient_field,
    "DT_IOP_BORDERS_ASPECT_ORIENTATION_AUTO", &auto_value));
  assert_true(dt_introspection_get_enum_value(orient_field,
    "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT", &portrait_value));
  assert_true(dt_introspection_get_enum_value(orient_field,
    "DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE", &landscape_value));

  const int states[] = { auto_value, portrait_value, landscape_value };
  const gboolean first_active[] = { FALSE, TRUE, FALSE };
  const gboolean first_writable[] = { TRUE, TRUE, FALSE };
  const gboolean second_active[] = { FALSE, TRUE, TRUE };
  const gboolean second_writable[] = { TRUE, FALSE, FALSE };
  for(guint i = 0; i < G_N_ELEMENTS(states); i++)
  {
    *orient = states[i];
    GHashTable *values = NULL;
    dt_remote_error_t *error = NULL;
    assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params,
                                             &values, &error));
    assert_null(error);
    dt_remote_vector_value_t *first = g_hash_table_lookup(values, "vector.first");
    dt_remote_vector_value_t *second = g_hash_table_lookup(values, "vector.second");
    assert_int_equal(first->active, first_active[i]);
    assert_int_equal(first->writable_now, first_writable[i]);
    assert_int_equal(second->active, second_active[i]);
    assert_int_equal(second->writable_now, second_writable[i]);
    g_hash_table_unref(values);
  }
  borders_fixture_free(fixture);
}
```

- [ ] **Step 2: Split projected-write gating into writable-only and active-only tests**

In the existing projected scalar-write test, use only the writable predicate:

```c
descriptor.active_when = NULL;
descriptor.writable_when = &predicate;
```

Retain its default rejection and same-patch PORTRAIT success. Add an active-only test using:

```c
descriptor.active_when = &predicate;
descriptor.writable_when = NULL;
```

The active-only test is:

```c
static void test_apply_patch_active_when_is_evaluated_independently(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  static const dt_remote_parameter_predicate_t predicate = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT",
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.active_when = &predicate;
  descriptor.writable_when = NULL;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);
  const double raw[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", raw, 3));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;

  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  assert_memory_equal(projected, before, fixture->module->params_size);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}
```

- [ ] **Step 3: Register and run the characterization tests**

Add:

```c
cmocka_unit_test_setup_teardown(
  test_read_values_evaluates_active_and_writable_predicates_independently,
  lookup_override_test_setup, lookup_override_test_teardown),
cmocka_unit_test_setup_teardown(
  test_apply_patch_active_when_is_evaluated_independently,
  lookup_override_test_setup, lookup_override_test_teardown),
```

Keep the existing registration for
`test_apply_patch_writable_when_evaluated_after_scalar_prepare_writes`.

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: PASS with the current implementation.

- [ ] **Step 4: Prove the tests are regression-sensitive, then restore production code**

Temporarily change the `read_values` writable evaluation to use `desc->active_when` and run the focused test. Expected: the new matrix fails. Restore the original `desc->writable_when` expression, rerun, and require PASS. Do not commit the temporary mutation.

- [ ] **Step 5: Commit**

```bash
git add src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector tests: distinguish active and writable predicates"
```

---

### Task 6: Complete schema and multi-descriptor read coverage (`VEC3-010`)

**Files:**
- Test: `src/tests/unittests/control/test_remote_vector.c:824-994,1475-1545`

**Interfaces:**
- Consumes: validated PLAIN and LEVELS descriptors; list-schema and read-values ownership contracts.
- Produces: full member-by-member schema assertions and successful two-descriptor read coverage.

- [ ] **Step 1: Configure a fully populated schema fixture**

Add valid predicates beside the schema test:

```c
static const dt_remote_parameter_predicate_t active_predicate = {
  .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
  .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT",
};
static const dt_remote_parameter_predicate_t writable_predicate = {
  .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_NE,
  .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE",
};
descriptors[0].description = "first description";
descriptors[0].active_when = &active_predicate;
descriptors[0].writable_when = &writable_predicate;
descriptors[1].subtype = DT_REMOTE_VECTOR_LEVELS;
descriptors[1].strictly_increasing = TRUE;
descriptors[1].minimum_gap = FLT_EPSILON;
descriptors[1].color_space = NULL;
```

- [ ] **Step 2: Assert every copied schema member and ownership boundary**

```c
assert_string_equal(first->display_name, "Color");
assert_string_equal(first->description, "first description");
assert_int_equal(first->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
assert_non_null(first->active_when);
assert_non_null(first->writable_when);
assert_ptr_not_equal(first->active_when, descriptors[0].active_when);
assert_ptr_not_equal(first->writable_when, descriptors[0].writable_when);
assert_string_equal(first->active_when->field, active_predicate.field);
assert_string_equal(first->writable_when->enum_name, writable_predicate.enum_name);
for(guint i = 0; i < descriptors[0].component_count; i++)
{
  const dt_remote_vector_component_schema_t component =
    g_array_index(first->components, dt_remote_vector_component_schema_t, i);
  assert_string_equal(component.name, descriptors[0].components[i].name);
  assert_float_equal(component.minimum, descriptors[0].components[i].minimum, 0.0);
  assert_float_equal(component.maximum, descriptors[0].components[i].maximum, 0.0);
}
assert_int_equal(second->subtype, DT_REMOTE_VECTOR_LEVELS);
assert_true(second->strictly_increasing);
assert_float_equal(second->minimum_gap, (double)FLT_EPSILON, 0.0);
assert_null(second->color_space);
```

Unref the returned array so the public destructor frees every optional field and owned component name.

- [ ] **Step 3: Add a native component writer for independent read setup**

```c
static void write_color_component(const borders_fixture_t *fixture,
                                  void *params,
                                  const char *field_name,
                                  guint index,
                                  float value)
{
  dt_introspection_field_t *array_field = NULL;
  void *array_ptr = dt_introspection_get_child(
    fixture->module->so->get_introspection()->field, params, field_name, &array_field);
  dt_introspection_field_t *element_field = NULL;
  float *element_ptr = dt_introspection_access_array(array_field, array_ptr, index,
                                                     &element_field);
  assert_non_null(element_ptr);
  assert_int_equal(element_field->header.type, DT_INTROSPECTION_TYPE_FLOAT);
  *element_ptr = value;
}
```

- [ ] **Step 4: Add a two-descriptor independent read test**

Install `vector.first` on `color` and `vector.second` on `frame_color`, then:

```c
static void test_read_values_reads_multiple_descriptors_independently(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  static dt_remote_vector_descriptor_t descriptors[2];
  descriptors[0] = make_color_descriptor();
  descriptors[0].name = "vector.first";
  descriptors[1] = make_color_descriptor();
  descriptors[1].name = "vector.second";
  descriptors[1].native = s_frame_color_path;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = descriptors, .vector_count = 2,
  };
  install_vector_adapter(&adapter);

const float color[] = { 0.1f, 0.2f, 0.3f };
const float frame[] = { 0.7f, 0.8f, 0.9f };
for(guint i = 0; i < 3; i++)
{
  write_color_component(fixture, fixture->module->params, "color", i, color[i]);
  write_color_component(fixture, fixture->module->params, "frame_color", i, frame[i]);
}
GHashTable *values = NULL;
dt_remote_error_t *error = NULL;
assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params,
                                         &values, &error));
assert_null(error);
dt_remote_vector_value_t *first = g_hash_table_lookup(values, "vector.first");
dt_remote_vector_value_t *second = g_hash_table_lookup(values, "vector.second");
assert_non_null(first);
assert_non_null(second);
assert_true(first->active);
assert_true(first->effective);
assert_true(first->writable_now);
assert_true(second->active);
assert_true(second->effective);
assert_true(second->writable_now);
for(guint i = 0; i < 3; i++)
{
  assert_float_equal(g_array_index(first->values, double, i), color[i], 0.0);
  assert_float_equal(g_array_index(second->values, double, i), frame[i], 0.0);
}
g_hash_table_unref(values);
  borders_fixture_free(fixture);
}
```

- [ ] **Step 5: Register and run focused tests**

Add:

```c
cmocka_unit_test_setup_teardown(
  test_read_values_reads_multiple_descriptors_independently,
  lookup_override_test_setup, lookup_override_test_teardown),
```

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: PASS; this task adds coverage without changing production behavior.

- [ ] **Step 6: Commit**

```bash
git add src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector tests: cover complete schemas and multi-vector reads"
```

---

### Task 7: Pin completion timing and registry write order (`VEC3-006`, `VEC3-010`)

**Files:**
- Test: `src/tests/unittests/control/test_remote_vector.c:555-575,1209-1425,1475-1545`

**Interfaces:**
- Consumes: `validate_completed(ctx, new_params, error)`, registry-ordered application, and caller-owned projected params.
- Produces: a callback that succeeds only after two native leaves contain their requested values, plus an alias-order test independent of request order.

- [ ] **Step 1: Add a completion callback that inspects both completed writes**

```c
static const float s_expected_completed_color[] = { 0.11f, 0.22f, 0.33f };
static const float s_expected_completed_frame[] = { 0.44f, 0.55f, 0.66f };
static gboolean s_validate_completed_force_reject = FALSE;

static gboolean validate_completed_observes_both_vectors(
  const struct dt_remote_vector_context_t *ctx,
  const void *new_params,
  dt_remote_error_t **error)
{
  s_validate_completed_calls++;
  const dt_remote_introspection_path_t paths[] = { s_color_path, s_frame_color_path };
  const float *expected[] = { s_expected_completed_color, s_expected_completed_frame };
  for(guint path_index = 0; path_index < G_N_ELEMENTS(paths); path_index++)
  {
    const dt_introspection_field_t *field = NULL;
    void *ptr = NULL;
    if(!dt_remote_path_resolve(&paths[path_index], ctx->introspection->field,
                               (void *)new_params, &field, &ptr, error))
      return FALSE;
    for(guint component = 0; component < 3; component++)
    {
      dt_introspection_field_t *element = NULL;
      const float *value = dt_introspection_access_array(
        (dt_introspection_field_t *)field, ptr, component, &element);
      if(!value || !element || *value != expected[path_index][component])
      {
        if(error)
        {
          *error = g_new0(dt_remote_error_t, 1);
          (*error)->code = DT_REMOTE_ERR_INVALID_VALUE;
          (*error)->message = g_strdup("validate_completed ran before both vector writes");
        }
        return FALSE;
      }
    }
  }
  if(s_validate_completed_force_reject)
  {
    if(error)
    {
      *error = g_new0(dt_remote_error_t, 1);
      (*error)->code = DT_REMOTE_ERR_INVALID_VALUE;
      (*error)->message = g_strdup("test rejected completed vector state");
    }
    return FALSE;
  }
  return TRUE;
}
```

- [ ] **Step 2: Add the two-write completion test**

Install `vector.color` on `s_color_path` and `vector.frame` on
`s_frame_color_path`, in that registry order:

```c
static void test_apply_patch_validate_completed_observes_all_writes(void **state)
{
  (void)state;
borders_fixture_t *fixture = borders_fixture_new();
static dt_remote_vector_descriptor_t descriptors[2];
descriptors[0] = make_color_descriptor();
descriptors[0].name = "vector.color";
descriptors[1] = make_color_descriptor();
descriptors[1].name = "vector.frame";
descriptors[1].native = s_frame_color_path;
static dt_remote_vector_module_adapter_t adapter;
adapter = (dt_remote_vector_module_adapter_t){
  .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
  .vectors = descriptors, .vector_count = 2,
  .validate_completed = validate_completed_observes_both_vectors,
};
install_vector_adapter(&adapter);

dt_remote_patch_t patch;
vector_patch_init(&patch);
void *projected = g_malloc(fixture->module->params_size);
dt_remote_error_t *error = NULL;
```

Add semantic patches in reverse request order using doubles converted from the expected floats:

```c
const double color_values[] = {
  s_expected_completed_color[0], s_expected_completed_color[1],
  s_expected_completed_color[2],
};
const double frame_values[] = {
  s_expected_completed_frame[0], s_expected_completed_frame[1],
  s_expected_completed_frame[2],
};
g_ptr_array_add(patch.semantic_values,
                make_vector_patch("vector.frame", frame_values, 3));
g_ptr_array_add(patch.semantic_values,
                make_vector_patch("vector.color", color_values, 3));
s_validate_completed_calls = 0;
assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
assert_null(error);
assert_int_equal(s_validate_completed_calls, 1);
```

In the same test, reuse the valid patch to exercise rejection after observation:

```c
void *live_before = g_malloc(fixture->module->params_size);
memcpy(live_before, fixture->module->params, fixture->module->params_size);
g_free(projected);
projected = g_malloc(fixture->module->params_size);
s_validate_completed_force_reject = TRUE;
s_validate_completed_calls = 0;
error = NULL;

assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
assert_non_null(error);
assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
assert_int_equal(s_validate_completed_calls, 1);
for(guint i = 0; i < 3; i++)
{
  assert_float_equal(read_color_component(fixture, projected, "color", i),
                     s_expected_completed_color[i], 0.0);
  assert_float_equal(read_color_component(fixture, projected, "frame_color", i),
                     s_expected_completed_frame[i], 0.0);
}
assert_memory_equal(fixture->module->params, live_before, fixture->module->params_size);

s_validate_completed_force_reject = FALSE;
dt_remote_error_free(error);
g_free(live_before);
g_free(projected);
vector_patch_cleanup(&patch);
borders_fixture_free(fixture);
}
```

- [ ] **Step 3: Add an alias test that makes registry order observable**

Install two names backed by `s_color_path`, ordered `vector.first`, then
`vector.second`:

```c
static void test_apply_patch_uses_registry_order_for_aliases(void **state)
{
  (void)state;
borders_fixture_t *fixture = borders_fixture_new();
static dt_remote_vector_descriptor_t aliases[2];
aliases[0] = make_color_descriptor();
aliases[0].name = "vector.first";
aliases[1] = make_color_descriptor();
aliases[1].name = "vector.second";
static dt_remote_vector_module_adapter_t alias_adapter;
alias_adapter = (dt_remote_vector_module_adapter_t){
  .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
  .vectors = aliases, .vector_count = 2,
};
install_vector_adapter(&alias_adapter);
dt_remote_patch_t patch;
vector_patch_init(&patch);
void *projected = g_malloc(fixture->module->params_size);
dt_remote_error_t *error = NULL;
```

Supply request entries in reverse order:

```c
const double first_values[] = { 0.1, 0.2, 0.3 };
const double second_values[] = { 0.7, 0.8, 0.9 };
g_ptr_array_add(patch.semantic_values,
                make_vector_patch("vector.second", second_values, 3));
g_ptr_array_add(patch.semantic_values,
                make_vector_patch("vector.first", first_values, 3));
assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
for(guint i = 0; i < 3; i++)
  assert_float_equal(read_color_component(fixture, projected, "color", i),
                     (float)second_values[i], 0.0);
assert_null(error);
g_free(projected);
vector_patch_cleanup(&patch);
borders_fixture_free(fixture);
}
```

- [ ] **Step 4: Register and run the characterization tests**

Add:

```c
cmocka_unit_test_setup_teardown(
  test_apply_patch_validate_completed_observes_all_writes,
  lookup_override_test_setup, lookup_override_test_teardown),
cmocka_unit_test_setup_teardown(
  test_apply_patch_uses_registry_order_for_aliases,
  lookup_override_test_setup, lookup_override_test_teardown),
```

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: PASS with the current post-loop callback placement.

- [ ] **Step 5: Prove callback timing sensitivity and restore production code**

Temporarily invoke `validate_completed` immediately before the descriptor write loop in `dt_remote_vector_apply_patch()`. Run the focused test and confirm the two-write completion test fails. Restore the original post-loop call, rerun, and require PASS. Do not commit the temporary mutation.

- [ ] **Step 6: Commit**

```bash
git add src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector tests: pin completion timing and registry write order"
```

---

### Task 8: Harden double-boundary and no-adapter coverage (`VEC3-008`, `VEC3-009`)

**Files:**
- Test: `src/tests/unittests/control/test_remote_vector.c:578-594,809-822,975-994,1108-1130,1181-1207,1452-1473`

**Interfaces:**
- Consumes: lookup override, double-domain apply validation, and no-adapter schema/read/apply behavior.
- Produces: tests that remain valid after Task 8 registers `borders`, plus an apply-level float-rounding boundary gate.

- [ ] **Step 1: Add an explicit always-no-adapter override**

```c
static const dt_remote_vector_module_adapter_t *always_null_vector_lookup(
  const char *operation, guint params_version)
{
  (void)operation;
  (void)params_version;
  return NULL;
}
```

Install it at the beginning of every test whose contract is “no adapter,” rather than relying on `s_adapters[]` being empty.

- [ ] **Step 2: Strengthen no-adapter output assertions**

Schema:

```c
GPtrArray *sentinel = g_ptr_array_new();
GPtrArray *fields = sentinel;
dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);
assert_true(dt_remote_vector_list_schema(so, &fields, &error));
assert_null(error);
assert_null(fields);
g_ptr_array_unref(sentinel);
```

Values:

```c
GHashTable *sentinel = g_hash_table_new(g_str_hash, g_str_equal);
GHashTable *values = sentinel;
dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);
assert_true(dt_remote_vector_read_values(&module, blob, &values, &error));
assert_null(error);
assert_ptr_not_equal(values, sentinel);
assert_int_equal(g_hash_table_size(values), 0);
g_hash_table_unref(values);
g_hash_table_unref(sentinel);
```

Apply: install the same override and retain `DT_REMOTE_ERR_UNKNOWN_FIELD` for a vector entry. For the scalar-only no-vector-content test, install the always-NULL override and retain successful no-op behavior.

- [ ] **Step 3: Replace the coarse apply-domain value with the float boundary**

In the domain-rejection test, use:

```c
const double values[] = { 0.1, 1.00000001, 0.2 };
```

After rejection, assert both live and projected blocks match the pre-call snapshot:

```c
assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
assert_non_null(strstr(error->details_json, "domain"));
assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
assert_memory_equal(projected, before, fixture->module->params_size);
```

- [ ] **Step 4: Register and run focused tests**

```bash
cmake --build build --target test_remote_vector -j2
ctest --test-dir build -R '^test_remote_vector$' --output-on-failure
```

Expected: PASS now and after a production `borders` adapter is registered.

- [ ] **Step 5: Commit**

```bash
git add src/tests/unittests/control/test_remote_vector.c
git commit -m "remote_vector tests: harden boundary and no-adapter coverage"
```

---

### Task 9: Verify closure and update the findings record

**Files:**
- Modify after review: `docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review.md`
- Track: `docs/superpowers/plans/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review-fixes.md`
- Verify: all Task 1–8 source and test files

**Interfaces:**
- Consumes: all prior task commits and their `VEC3-*` mapping.
- Produces: fresh build/test/review evidence and a closed findings record; no production interface change.

- [ ] **Step 1: Run the focused build**

```bash
cmake --build build --target \
  test_remote_vector \
  test_remote_curve \
  test_remote_curve_registry \
  test_remote_edit \
  test_remote_protocol -j2
```

Expected: build exits 0 with no warning promoted to an error.

- [ ] **Step 2: Run focused vector and curve regressions**

```bash
ctest --test-dir build \
  -R '^test_remote_(vector|curve|curve_registry|edit|protocol)$' \
  --output-on-failure
```

Expected: every selected target passes.

- [ ] **Step 3: Run the complete configured CTest suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: 100% pass, zero failed tests.

- [ ] **Step 4: Check formatting and worktree scope**

```bash
git diff --check ffd00e2d8a..HEAD
git status --short
git diff --stat ffd00e2d8a..HEAD
```

Expected: no whitespace errors; only vector-core production/tests and the two approved Milestone 4 Task 3 review documents are changed by this remediation.

- [ ] **Step 5: Request a fresh code review**

Use `superpowers:requesting-code-review` with base `edb40b6a2e`, head `HEAD`, the approved findings document, and this plan. Require the reviewer to verify every `VEC3-*` closure test and report Critical/Important/Minor findings. Do not mark the review record closed while any Critical or Important finding remains.

- [ ] **Step 6: Update the review record after the review gate passes**

Change the document header to:

```markdown
- **Status:** Closed — remediated and re-reviewed before Milestone 4 Task 4
```

Change every summary-table status from `Open` to `Closed`, and append the remediation commit range plus exact focused/full test results under `Verification evidence`. Do not rewrite the original evidence or required-resolution sections; they remain the audit trail.

- [ ] **Step 7: Commit the closure record**

```bash
git add docs/superpowers/specs/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review.md
git add docs/superpowers/plans/2026-07-12-darktable-mcp-milestone4-task3-vector-core-review-fixes.md
git commit -m "darktable-mcp: close milestone 4 task 3 review findings"
```

## Final acceptance checklist

- [ ] VEC3-001: malformed adapter/descriptor metadata returns `DT_REMOTE_ERR_INTERNAL` without a crash or partial output.
- [ ] VEC3-002: PLAIN and LEVELS validators reject every non-finite component before narrowing.
- [ ] VEC3-003: cross-class collisions follow current override state and never poison intrinsic cache entries.
- [ ] VEC3-004: subtype metadata is exhaustive and internally consistent.
- [ ] VEC3-005: tests distinguish active and writable predicate evaluation.
- [ ] VEC3-006: `validate_completed` observes every registry-ordered write and caller rollback remains explicit.
- [ ] VEC3-007: missing paths, wrong leaf types, missing element metadata, count overflow, and capacity mismatch are isolated.
- [ ] VEC3-008: apply rejects a double just above the component maximum even when it rounds into range as float.
- [ ] VEC3-009: no-adapter tests use an explicit always-NULL override and survive Task 8 registry growth.
- [ ] VEC3-010: full schema ownership plus multi-descriptor read/apply loops are covered.
- [ ] Focused vector/curve/edit/protocol tests pass.
- [ ] Full configured CTest suite passes.
- [ ] Fresh review reports no Critical or Important findings.
