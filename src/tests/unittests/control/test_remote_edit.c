/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
 * cmocka unit tests for the remote-edit pure introspection-conversion
 * layer (src/control/remote_edit.c).
 *
 * These tests exercise dt_remote_schema_from_introspection(),
 * dt_remote_value_from_field() and dt_remote_value_validate_and_write()
 * against hand-built fixture dt_introspection_field_t arrays -- no
 * dt_develop_t, no live darkroom. Live-module traversal (dt_remote_get_state
 * and friends) is covered by integration tests in a later step.
 *
 * Please see README.md for more detailed documentation.
 */
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_edit.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/*
 * FIXTURE
 *
 * A small hand-built params struct + introspection descriptor, modelled
 * on the shape the code generator produces for real modules (see e.g.
 * build/lib/darktable/plugins/introspection_dither.c): leaf entries in
 * declaration order, a STRUCT summary entry for the nested "random"
 * struct right after its own leaves, and a final STRUCT entry
 * summarizing the whole params struct, terminated by a NONE sentinel.
 */

typedef struct fixture_random_t
{
  float radius;
  float damping;
} fixture_random_t;

typedef struct fixture_params_t
{
  gboolean enabled_flag;
  int count;
  float amount;
  int mode;                 // backing storage for the "mode" enum field
  fixture_random_t random;
  float coeffs[3];
} fixture_params_t;

enum { FIXTURE_MODE_A = 0, FIXTURE_MODE_B = 1 };

static dt_introspection_type_enum_tuple_t fixture_mode_values[] = {
  { "FIXTURE_MODE_A", FIXTURE_MODE_A, "mode a" },
  { "FIXTURE_MODE_B", FIXTURE_MODE_B, "mode b" },
  { NULL, 0, NULL },
};

enum
{
  FIXTURE_IDX_ENABLED_FLAG = 0,
  FIXTURE_IDX_COUNT,
  FIXTURE_IDX_AMOUNT,
  FIXTURE_IDX_MODE,
  FIXTURE_IDX_RANDOM_RADIUS,
  FIXTURE_IDX_RANDOM_DAMPING,
  FIXTURE_IDX_RANDOM_STRUCT,
  FIXTURE_IDX_COEFFS_ARRAY,
  FIXTURE_IDX_TOP_STRUCT,
  FIXTURE_IDX_NONE,
  FIXTURE_LINEAR_COUNT
};

static dt_introspection_field_t fixture_linear[FIXTURE_LINEAR_COUNT] = {
  [FIXTURE_IDX_ENABLED_FLAG] = { .Bool = {
      { DT_INTROSPECTION_TYPE_BOOL, "gboolean", "enabled_flag", "enabled_flag", "enable it",
        sizeof(gboolean), offsetof(fixture_params_t, enabled_flag), NULL },
      TRUE } },
  [FIXTURE_IDX_COUNT] = { .Int = {
      { DT_INTROSPECTION_TYPE_INT, "int", "count", "count", "",
        sizeof(int), offsetof(fixture_params_t, count), NULL },
      -10, 10, 1 } },
  [FIXTURE_IDX_AMOUNT] = { .Float = {
      { DT_INTROSPECTION_TYPE_FLOAT, "float", "amount", "amount", "the amount",
        sizeof(float), offsetof(fixture_params_t, amount), NULL },
      0.0f, 1.0f, 0.5f } },
  [FIXTURE_IDX_MODE] = { .Enum = {
      { DT_INTROSPECTION_TYPE_ENUM, "fixture_mode_t", "mode", "mode", "the mode",
        sizeof(int), offsetof(fixture_params_t, mode), NULL },
      2, fixture_mode_values, FIXTURE_MODE_B } },
  [FIXTURE_IDX_RANDOM_RADIUS] = { .Float = {
      { DT_INTROSPECTION_TYPE_FLOAT, "float", "random.radius", "radius", "",
        sizeof(float), offsetof(fixture_params_t, random.radius), NULL },
      0.0f, 100.0f, 5.0f } },
  [FIXTURE_IDX_RANDOM_DAMPING] = { .Float = {
      { DT_INTROSPECTION_TYPE_FLOAT, "float", "random.damping", "damping", "damping desc",
        sizeof(float), offsetof(fixture_params_t, random.damping), NULL },
      -200.0f, 0.0f, -100.0f } },
  [FIXTURE_IDX_RANDOM_STRUCT] = { .Struct = {
      { DT_INTROSPECTION_TYPE_STRUCT, "fixture_random_t", "random", "random", "",
        sizeof(fixture_random_t), offsetof(fixture_params_t, random), NULL },
      2, NULL } },
  [FIXTURE_IDX_COEFFS_ARRAY] = { .Array = {
      { DT_INTROSPECTION_TYPE_ARRAY, "float[3]", "coeffs", "coeffs", "",
        sizeof(float) * 3, offsetof(fixture_params_t, coeffs), NULL },
      3, DT_INTROSPECTION_TYPE_FLOAT, NULL } },
  [FIXTURE_IDX_TOP_STRUCT] = { .Struct = {
      { DT_INTROSPECTION_TYPE_STRUCT, "fixture_params_t", "", "", "",
        sizeof(fixture_params_t), 0, NULL },
      6, NULL } },
  [FIXTURE_IDX_NONE] = { .header = { DT_INTROSPECTION_TYPE_NONE, NULL, NULL, NULL, NULL, 0, 0, NULL } },
};

static dt_remote_field_t *find_field(GPtrArray *fields, const char *name)
{
  for(guint i = 0; i < fields->len; i++)
  {
    dt_remote_field_t *f = g_ptr_array_index(fields, i);
    if(!g_strcmp0(f->name, name)) return f;
  }
  return NULL;
}

/*
 * dt_remote_value_from_field()
 */

static void test_value_from_field_float(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.75f;

  dt_remote_value_t v;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_AMOUNT], &params, &v));
  assert_int_equal(v.type, DT_REMOTE_VALUE_FLOAT);
  assert_float_equal(v.v.f, 0.75, 1e-9);
}

static void test_value_from_field_int(void **state)
{
  fixture_params_t params = { 0 };
  params.count = -3;

  dt_remote_value_t v;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_COUNT], &params, &v));
  assert_int_equal(v.type, DT_REMOTE_VALUE_INT);
  assert_int_equal(v.v.i, -3);
}

static void test_value_from_field_bool(void **state)
{
  fixture_params_t params = { 0 };
  params.enabled_flag = TRUE;

  dt_remote_value_t v;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_ENABLED_FLAG], &params, &v));
  assert_int_equal(v.type, DT_REMOTE_VALUE_BOOL);
  assert_true(v.v.b);
}

static void test_value_from_field_enum(void **state)
{
  fixture_params_t params = { 0 };
  params.mode = FIXTURE_MODE_B;

  dt_remote_value_t v;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_MODE], &params, &v));
  assert_int_equal(v.type, DT_REMOTE_VALUE_ENUM);
  assert_int_equal(v.v.e.value, FIXTURE_MODE_B);
  assert_string_equal(v.v.e.name, "FIXTURE_MODE_B");
  dt_remote_value_clear(&v);
}

static void test_value_from_field_nested_struct_leaf_resolves_offset(void **state)
{
  fixture_params_t params = { 0 };
  params.random.radius = 12.5f;
  params.random.damping = -42.0f;

  dt_remote_value_t radius, damping;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_RANDOM_RADIUS], &params, &radius));
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_RANDOM_DAMPING], &params, &damping));
  assert_float_equal(radius.v.f, 12.5, 1e-9);
  assert_float_equal(damping.v.f, -42.0, 1e-9);
}

static void test_value_from_field_unsupported_type_returns_false(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_value_t v;
  assert_false(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_COEFFS_ARRAY], &params, &v));
  assert_false(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_RANDOM_STRUCT], &params, &v));
  assert_false(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_TOP_STRUCT], &params, &v));
}

static void test_repeated_calls_do_not_alias_params_memory(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.25f;
  params.mode = FIXTURE_MODE_A;

  dt_remote_value_t v1, v2;
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_AMOUNT], &params, &v1));

  // mutating the live params blob after the read must not affect the
  // already-extracted (copied-by-value) result.
  params.amount = 99.0f;
  assert_float_equal(v1.v.f, 0.25, 1e-9);

  // two independent enum reads own two independent, separately freeable
  // name strings -- no shared/static buffer aliasing.
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_MODE], &params, &v1));
  assert_true(dt_remote_value_from_field(&fixture_linear[FIXTURE_IDX_MODE], &params, &v2));
  assert_string_equal(v1.v.e.name, v2.v.e.name);
  assert_ptr_not_equal(v1.v.e.name, v2.v.e.name);
  dt_remote_value_clear(&v1);
  assert_string_equal(v2.v.e.name, "FIXTURE_MODE_A");
  dt_remote_value_clear(&v2);
}

/*
 * dt_remote_value_validate_and_write()
 */

static void test_validate_and_write_float_in_range(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_value_t v = { .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.9 };
  dt_remote_error_t *err = NULL;

  assert_true(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_AMOUNT], &v, &params, &err));
  assert_null(err);
  assert_float_equal(params.amount, 0.9, 1e-6);
}

static void test_validate_and_write_float_out_of_range_rejected(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.5f;
  dt_remote_value_t v = { .type = DT_REMOTE_VALUE_FLOAT, .v.f = 42.0 };
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_AMOUNT], &v, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_float_equal(params.amount, 0.5, 1e-9);  // untouched on rejection
  dt_remote_error_free(err);
}

static void test_validate_and_write_float_nan_rejected(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.5f;
  dt_remote_value_t v = { .type = DT_REMOTE_VALUE_FLOAT, .v.f = NAN };
  dt_remote_error_t *err = NULL;

  // `v.v.f < min || v.v.f > max` would accept NaN (both comparisons are
  // false); the range check must instead require `>= min && <= max` so
  // NaN is rejected as out-of-range/invalid.
  assert_false(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_AMOUNT], &v, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_float_equal(params.amount, 0.5, 1e-9);  // untouched on rejection
  dt_remote_error_free(err);
}

static void test_validate_and_write_wrong_value_type_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_value_t v = { .type = DT_REMOTE_VALUE_INT, .v.i = 1 };
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_AMOUNT], &v, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);
}

static void test_validate_and_write_enum_valid_and_invalid(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_error_t *err = NULL;

  dt_remote_value_t good = { .type = DT_REMOTE_VALUE_ENUM, .v.e = { FIXTURE_MODE_A, NULL } };
  assert_true(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_MODE], &good, &params, &err));
  assert_int_equal(params.mode, FIXTURE_MODE_A);

  dt_remote_value_t bad = { .type = DT_REMOTE_VALUE_ENUM, .v.e = { 999, NULL } };
  assert_false(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_MODE], &bad, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_int_equal(params.mode, FIXTURE_MODE_A);  // untouched on rejection
  dt_remote_error_free(err);
}

static void test_validate_and_write_unsupported_field_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_value_t v = { .type = DT_REMOTE_VALUE_FLOAT, .v.f = 1.0 };
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_value_validate_and_write(&fixture_linear[FIXTURE_IDX_COEFFS_ARRAY], &v, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(err);
}

/*
 * dt_remote_schema_from_introspection()
 */

static void test_schema_includes_every_linear_entry_except_none(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);
  assert_int_equal(fields->len, FIXTURE_LINEAR_COUNT - 1);
  g_ptr_array_unref(fields);
}

static void test_schema_scalar_types_map_to_expected_value_classes(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);

  dt_remote_field_t *f;

  f = find_field(fields, "enabled_flag");
  assert_non_null(f);
  assert_string_equal(f->type_name, "bool");
  assert_true(f->writable);
  assert_int_equal(f->default_value.type, DT_REMOTE_VALUE_BOOL);
  assert_true(f->default_value.v.b);

  f = find_field(fields, "count");
  assert_non_null(f);
  assert_string_equal(f->type_name, "int");
  assert_true(f->writable);
  assert_int_equal(f->default_value.type, DT_REMOTE_VALUE_INT);
  assert_int_equal(f->default_value.v.i, 1);

  f = find_field(fields, "amount");
  assert_non_null(f);
  assert_string_equal(f->type_name, "float");
  assert_true(f->writable);
  assert_int_equal(f->default_value.type, DT_REMOTE_VALUE_FLOAT);
  assert_float_equal(f->default_value.v.f, 0.5, 1e-9);

  f = find_field(fields, "mode");
  assert_non_null(f);
  assert_string_equal(f->type_name, "enum");
  assert_true(f->writable);
  assert_int_equal(f->default_value.type, DT_REMOTE_VALUE_ENUM);
  assert_int_equal(f->default_value.v.e.value, FIXTURE_MODE_B);
  assert_string_equal(f->default_value.v.e.name, "FIXTURE_MODE_B");

  g_ptr_array_unref(fields);
}

static void test_schema_nested_struct_leaves_use_dotted_names(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);

  dt_remote_field_t *radius = find_field(fields, "random.radius");
  dt_remote_field_t *damping = find_field(fields, "random.damping");
  assert_non_null(radius);
  assert_non_null(damping);
  assert_true(radius->writable);
  assert_true(damping->writable);
  assert_float_equal(damping->minimum, -200.0, 1e-9);
  assert_float_equal(damping->maximum, 0.0, 1e-9);
  assert_float_equal(damping->default_value.v.f, -100.0, 1e-9);

  g_ptr_array_unref(fields);
}

static void test_schema_ranges_and_defaults_preserved(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);

  dt_remote_field_t *f = find_field(fields, "count");
  assert_non_null(f);
  assert_true(f->has_range);
  assert_float_equal(f->minimum, -10.0, 1e-9);
  assert_float_equal(f->maximum, 10.0, 1e-9);

  f = find_field(fields, "amount");
  assert_true(f->has_range);
  assert_float_equal(f->minimum, 0.0, 1e-9);
  assert_float_equal(f->maximum, 1.0, 1e-9);

  g_ptr_array_unref(fields);
}

static void test_schema_enum_members_preserved(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);

  dt_remote_field_t *f = find_field(fields, "mode");
  assert_non_null(f);
  assert_non_null(f->enum_values);
  assert_int_equal(f->enum_values->len, 2);

  dt_remote_enum_value_t *e0 = g_ptr_array_index(f->enum_values, 0);
  dt_remote_enum_value_t *e1 = g_ptr_array_index(f->enum_values, 1);
  assert_string_equal(e0->name, "FIXTURE_MODE_A");
  assert_int_equal(e0->value, FIXTURE_MODE_A);
  assert_string_equal(e1->name, "FIXTURE_MODE_B");
  assert_int_equal(e1->value, FIXTURE_MODE_B);

  g_ptr_array_unref(fields);
}

static void test_schema_unsupported_types_marked_writable_false_not_omitted(void **state)
{
  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, NULL);

  dt_remote_field_t *array_field = find_field(fields, "coeffs");
  dt_remote_field_t *nested_struct = find_field(fields, "random");
  assert_non_null(array_field);
  assert_non_null(nested_struct);
  assert_false(array_field->writable);
  assert_false(nested_struct->writable);
  assert_string_equal(array_field->type_name, "array");
  assert_string_equal(nested_struct->type_name, "struct");

  // the top-level params struct entry (name == "") is present too --
  // "never omitted" applies even to it.
  gboolean saw_top_level = FALSE;
  for(guint i = 0; i < fields->len; i++)
  {
    dt_remote_field_t *f = g_ptr_array_index(fields, i);
    if(!g_strcmp0(f->name, "") && !g_strcmp0(f->type_name, "struct"))
      saw_top_level = TRUE;
  }
  assert_true(saw_top_level);

  g_ptr_array_unref(fields);
}

static void test_schema_denylist_forces_writable_false(void **state)
{
  static const char *const denylist_names[] = { "amount", NULL };
  dt_remote_denylist_t denylist = { .names = denylist_names };

  GPtrArray *fields = dt_remote_schema_from_introspection(fixture_linear, &denylist);

  dt_remote_field_t *amount = find_field(fields, "amount");
  dt_remote_field_t *count = find_field(fields, "count");
  assert_non_null(amount);
  assert_non_null(count);
  assert_false(amount->writable);           // denylisted despite being a float
  assert_string_equal(amount->type_name, "float");  // type classification unaffected
  assert_true(count->writable);             // untouched by an unrelated denylist entry

  g_ptr_array_unref(fields);
}

/*
 * dt_remote_patch_apply() -- the pure core of dt_remote_set_module_params
 * (plan step 7, internals doc §3 steps 4-6). No dt_develop_t/live module
 * involved: params_blob is just a fixture_params_t on the stack, standing
 * in for the g_malloc()'d scratch copy the live wrapper allocates.
 */

static dt_remote_patch_entry_t *make_entry(const char *name, dt_remote_value_t value)
{
  dt_remote_patch_entry_t *entry = g_malloc0(sizeof(dt_remote_patch_entry_t));
  entry->name = g_strdup(name);
  entry->value = value;
  return entry;
}

static void test_patch_apply_single_valid_field(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.1f;

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.9 }));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_null(err);
  assert_float_equal(params.amount, 0.9, 1e-9);

  g_ptr_array_unref(patch.scalar_values);
}

// "multiple valid fields create one history item" (plan step 7's test
// list): the "one history item" half is a live-commit property the
// integration suite covers (dt_remote_set_module_params calls
// dt_dev_add_history_item() exactly once, after this whole loop, by
// construction -- see remote_edit.c). This is the pure half: every field
// in a multi-entry patch is actually applied by one dt_remote_patch_apply()
// call.
static void test_patch_apply_multiple_valid_fields(void **state)
{
  fixture_params_t params = { 0 };

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.75 }));
  g_ptr_array_add(patch.scalar_values,
                  make_entry("count", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_INT, .v.i = 7 }));
  g_ptr_array_add(patch.scalar_values,
                  make_entry("enabled_flag", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_BOOL, .v.b = TRUE }));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_null(err);
  assert_float_equal(params.amount, 0.75, 1e-9);
  assert_int_equal(params.count, 7);
  assert_true(params.enabled_flag);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_enum_by_int_and_by_name(void **state)
{
  // "by int": constructed directly with the numeric value, as
  // remote_protocol.c's JSON conversion does for a plain integer.
  {
    fixture_params_t params = { 0 };
    dt_remote_patch_t patch = { 0 };
    patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
    g_ptr_array_add(patch.scalar_values,
                    make_entry("mode", (dt_remote_value_t){
                      .type = DT_REMOTE_VALUE_ENUM, .v.e = { FIXTURE_MODE_B, NULL } }));

    dt_remote_error_t *err = NULL;
    assert_true(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
    assert_null(err);
    assert_int_equal(params.mode, FIXTURE_MODE_B);

    g_ptr_array_unref(patch.scalar_values);
  }

  // "by stable name": remote_protocol.c resolves a JSON string like
  // "FIXTURE_MODE_A" to its integer value against the schema before
  // building the dt_remote_value_t -- dt_remote_patch_apply() itself only
  // ever sees the resolved (value, name) pair, exercised here directly.
  {
    fixture_params_t params = { 0 };
    params.mode = FIXTURE_MODE_B;
    dt_remote_patch_t patch = { 0 };
    patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
    g_ptr_array_add(patch.scalar_values,
                    make_entry("mode", (dt_remote_value_t){
                      .type = DT_REMOTE_VALUE_ENUM, .v.e = { FIXTURE_MODE_A, g_strdup("FIXTURE_MODE_A") } }));

    dt_remote_error_t *err = NULL;
    assert_true(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
    assert_null(err);
    assert_int_equal(params.mode, FIXTURE_MODE_A);

    g_ptr_array_unref(patch.scalar_values);
  }
}

// "any invalid field leaves every field unchanged": the second entry is
// invalid, so the whole patch is rejected -- dt_remote_patch_apply()
// itself only guarantees params_blob is scratch-safe to discard on
// failure; the live wrapper is what turns that into "module->params
// literally never touched" by never memcpy-ing this blob back. Verified
// here at the level dt_remote_patch_apply() controls: failure, correct
// error code, and no further entries processed.
static void test_patch_apply_any_invalid_field_rejects_whole_patch(void **state)
{
  fixture_params_t params = { 0 };
  params.amount = 0.2f;
  params.count = 3;

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.5 }));
  g_ptr_array_add(patch.scalar_values,
                  make_entry("count", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_INT, .v.i = 999 }));  // out of range

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_unknown_field_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("does_not_exist", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 1.0 }));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_UNKNOWN_FIELD);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_unsupported_field_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  // "random" is a known field (the nested struct summary entry) but not a
  // writable scalar leaf.
  g_ptr_array_add(patch.scalar_values,
                  make_entry("random", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 1.0 }));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_denylisted_field_rejected(void **state)
{
  static const char *const denylist_names[] = { "amount", NULL };
  dt_remote_denylist_t denylist = { .names = denylist_names };

  fixture_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.5 }));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, &denylist, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_duplicate_field_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.1 }));
  g_ptr_array_add(patch.scalar_values,
                  make_entry("amount", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.2 }));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_empty_patch_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

static void test_patch_apply_null_patch_rejected(void **state)
{
  fixture_params_t params = { 0 };
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(fixture_linear, NULL, NULL, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_value_from_field_float),
    cmocka_unit_test(test_value_from_field_int),
    cmocka_unit_test(test_value_from_field_bool),
    cmocka_unit_test(test_value_from_field_enum),
    cmocka_unit_test(test_value_from_field_nested_struct_leaf_resolves_offset),
    cmocka_unit_test(test_value_from_field_unsupported_type_returns_false),
    cmocka_unit_test(test_repeated_calls_do_not_alias_params_memory),
    cmocka_unit_test(test_validate_and_write_float_in_range),
    cmocka_unit_test(test_validate_and_write_float_out_of_range_rejected),
    cmocka_unit_test(test_validate_and_write_float_nan_rejected),
    cmocka_unit_test(test_validate_and_write_wrong_value_type_rejected),
    cmocka_unit_test(test_validate_and_write_enum_valid_and_invalid),
    cmocka_unit_test(test_validate_and_write_unsupported_field_rejected),
    cmocka_unit_test(test_schema_includes_every_linear_entry_except_none),
    cmocka_unit_test(test_schema_scalar_types_map_to_expected_value_classes),
    cmocka_unit_test(test_schema_nested_struct_leaves_use_dotted_names),
    cmocka_unit_test(test_schema_ranges_and_defaults_preserved),
    cmocka_unit_test(test_schema_enum_members_preserved),
    cmocka_unit_test(test_schema_unsupported_types_marked_writable_false_not_omitted),
    cmocka_unit_test(test_schema_denylist_forces_writable_false),

    cmocka_unit_test(test_patch_apply_single_valid_field),
    cmocka_unit_test(test_patch_apply_multiple_valid_fields),
    cmocka_unit_test(test_patch_apply_enum_by_int_and_by_name),
    cmocka_unit_test(test_patch_apply_any_invalid_field_rejects_whole_patch),
    cmocka_unit_test(test_patch_apply_unknown_field_rejected),
    cmocka_unit_test(test_patch_apply_unsupported_field_rejected),
    cmocka_unit_test(test_patch_apply_denylisted_field_rejected),
    cmocka_unit_test(test_patch_apply_duplicate_field_rejected),
    cmocka_unit_test(test_patch_apply_empty_patch_rejected),
    cmocka_unit_test(test_patch_apply_null_patch_rejected),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
