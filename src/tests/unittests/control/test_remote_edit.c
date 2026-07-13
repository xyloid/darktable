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
 * and friends) is covered by integration tests in a later step. The one
 * exception is dt_remote_get_module_schema() itself: it reads the loaded
 * module .so table (darktable.iop), which needs a real (GUI-less,
 * data-less) dt_init() -- see the harness right before main() below. That
 * still isn't a live darkroom/dt_develop_t, just the module registry.
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

#include "common/darktable.h"
#include "control/remote_curve.h"
#include "control/remote_edit.h"
#include "control/remote_vector.h"
#include "develop/develop.h"
#include "develop/imageop.h"  // dt_iop_get_module_so()/dt_iop_module_so_t: real-module
                              // denylist-wiring regression test below

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
 * dt_remote_denylist_for_op() -- the static per-op forced-writable:false
 * table (supported-operations appendix). Pure lookup, no live module
 * needed.
 */

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

/*
 * dt_remote_get_module_schema() -- the ONE test in this file that needs a
 * real loaded module .so instead of the fixture_linear array above.
 * dt_remote_get_module_schema() reads darktable.iop (via
 * dt_iop_get_module_so()) but, per its own header comment, needs no
 * dt_develop_t/live darkroom -- schemas are per-op-from-the-.so. So we run
 * the smallest dt_init() that populates darktable.iop: GUI and image
 * library both off, and a throwaway config dir so the suite never touches
 * the developer's real ~/.config/darktable. DT_TEST_MODULEDIR (see
 * CMakeLists.txt) points dt_init() at this build's own plugin directory --
 * without it, dt_init() looks for plugins next to the test binary instead
 * of in the build tree and finds none.
 */

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_harness_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *error = NULL;
  s_harness_confdir = g_dir_make_tmp("test_remote_edit-XXXXXX", &error);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_edit: failed to create scratch config dir: %s\n",
            error->message);
    g_error_free(error);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_edit",
    "--configdir", s_harness_confdir,
    "--library", ":memory:",
    "--moduledir", DT_TEST_MODULEDIR,
    "--conf", "write_sidecar_files=never",
    NULL
  };
  int argc_override = G_N_ELEMENTS(argv_override) - 1;
  return dt_init(argc_override, argv_override, FALSE, FALSE, NULL) ? -1 : 0;
}

static int harness_group_teardown(void **state)
{
  (void)state;
  dt_cleanup();
  if(s_harness_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_harness_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_edit: failed to remove scratch config dir %s\n",
              s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

/* ---------------------------------------------------------------------- */
/* rgbcurve scratch-transaction fixture                                   */
/* ---------------------------------------------------------------------- */

typedef struct live_rgbcurve_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
  guint history_items;
} live_rgbcurve_fixture_t;

static live_rgbcurve_fixture_t *live_rgbcurve_fixture_new(void)
{
  live_rgbcurve_fixture_t *fixture = g_new0(live_rgbcurve_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;

  fixture->module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  assert_false(dt_iop_load_module(fixture->module, so, &fixture->dev));
  memcpy(fixture->module->params, fixture->module->default_params,
         fixture->module->params_size);
  fixture->dev.iop = g_list_append(fixture->dev.iop, fixture->module);
  return fixture;
}

static void live_rgbcurve_fixture_free(live_rgbcurve_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static int live_rgbcurve_test_setup(void **state)
{
  *state = live_rgbcurve_fixture_new();
  return 0;
}

static int live_rgbcurve_test_teardown(void **state)
{
  live_rgbcurve_fixture_free(*state);
  *state = NULL;
  return 0;
}

static int live_rgbcurve_enum_value(live_rgbcurve_fixture_t *fixture,
                                    const char *field_name,
                                    const char *enum_name)
{
  dt_introspection_t *intro = fixture->module->so->get_introspection();
  dt_introspection_field_t *field = NULL;
  void *ptr = dt_introspection_get_child(intro->field, fixture->module->params,
                                         field_name, &field);
  assert_non_null(ptr);
  assert_non_null(field);
  int value = 0;
  assert_true(dt_introspection_get_enum_value(field, enum_name, &value));
  return value;
}

static dt_remote_patch_entry_t *live_rgbcurve_enum_entry(live_rgbcurve_fixture_t *fixture,
                                                         const char *field_name,
                                                         const char *enum_name)
{
  dt_remote_patch_entry_t *entry = make_entry(field_name, (dt_remote_value_t){
    .type = DT_REMOTE_VALUE_ENUM,
    .v.e = { .value = live_rgbcurve_enum_value(fixture, field_name, enum_name),
             .name = g_strdup(enum_name) },
  });
  return entry;
}

static dt_remote_semantic_patch_t *live_rgbcurve_curve_patch(
  const char *name,
  const dt_remote_curve_point_t *points,
  guint count,
  gboolean has_interpolation,
  dt_remote_curve_interpolation_t interpolation)
{
  dt_remote_semantic_patch_t *semantic = g_new0(dt_remote_semantic_patch_t, 1);
  semantic->class_id = DT_REMOTE_PARAMETER_CURVE;
  semantic->value.curve.name = g_strdup(name);
  semantic->value.curve.points = g_array_sized_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t), count);
  g_array_append_vals(semantic->value.curve.points, points, count);
  semantic->value.curve.has_interpolation = has_interpolation;
  semantic->value.curve.interpolation = interpolation;
  return semantic;
}

static void live_rgbcurve_patch_init(dt_remote_patch_t *patch)
{
  memset(patch, 0, sizeof(*patch));
  patch->scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  patch->semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
}

static void live_rgbcurve_patch_cleanup(dt_remote_patch_t *patch)
{
  g_ptr_array_unref(patch->scalar_values);
  g_ptr_array_unref(patch->semantic_values);
}

static gboolean apply_scratch_transaction(live_rgbcurve_fixture_t *fixture,
                                          const dt_remote_patch_t *patch,
                                          gboolean stale_revision,
                                          dt_remote_error_t **error)
{
  if(stale_revision)
  {
    if(error)
    {
      *error = g_new0(dt_remote_error_t, 1);
      (*error)->code = DT_REMOTE_ERR_REVISION_CONFLICT;
      (*error)->message = g_strdup("stale fixture revision");
    }
    return FALSE;
  }

  void *projected = g_malloc(fixture->module->params_size);
  memcpy(projected, fixture->module->params, fixture->module->params_size);
  dt_introspection_field_t *linear = fixture->module->so->get_introspection_linear();
  const gboolean ok =
    dt_remote_patch_apply(linear, dt_remote_denylist_for_op("rgbcurve"), patch, projected, error)
    && dt_remote_curve_apply_patch(fixture->module, fixture->module->params,
                                   projected, patch, error);
  if(ok)
  {
    memcpy(fixture->module->params, projected, fixture->module->params_size);
    if(patch->has_enable) fixture->module->enabled = patch->enable;
    fixture->history_items++;
  }
  g_free(projected);
  return ok;
}

static void assert_live_curve_points(live_rgbcurve_fixture_t *fixture,
                                     const char *name,
                                     const dt_remote_curve_point_t *expected,
                                     guint count)
{
  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_read_values(fixture->module, fixture->module->params,
                                          &values, &error));
  assert_null(error);
  dt_remote_curve_value_t *value = g_hash_table_lookup(values, name);
  assert_non_null(value);
  assert_int_equal(value->points->len, count);
  for(guint i = 0; i < count; i++)
  {
    const dt_remote_curve_point_t actual =
      g_array_index(value->points, dt_remote_curve_point_t, i);
    assert_float_equal(actual.x, expected[i].x, 1e-6);
    assert_float_equal(actual.y, expected[i].y, 1e-6);
  }
  g_hash_table_unref(values);
}

static void test_transaction_scalar_plus_curve_patch_commits_once(void **state)
{
  static const dt_remote_curve_point_t red_points[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.45, .y = 0.58 }, { .x = 1.0, .y = 1.0 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  live_rgbcurve_enum_entry(fixture, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB"));
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.red", red_points, G_N_ELEMENTS(red_points), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));

  dt_remote_error_t *error = NULL;
  assert_true(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_null(error);
  assert_int_equal(fixture->history_items, 1);
  assert_live_curve_points(fixture, "curve.red", red_points, G_N_ELEMENTS(red_points));

  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_any_invalid_scalar_curve_leaves_every_field_unchanged(void **state)
{
  static const dt_remote_curve_point_t invalid_green[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.0025, .y = 0.2 }, { .x = 1.0, .y = 1.0 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);
  const gboolean enabled_before = fixture->module->enabled;

  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  live_rgbcurve_enum_entry(fixture, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB"));
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.green", invalid_green,
                                            G_N_ELEMENTS(invalid_green), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  patch.has_enable = TRUE;
  patch.enable = !enabled_before;

  dt_remote_error_t *error = NULL;
  assert_false(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_int_equal(fixture->module->enabled, enabled_before);
  assert_int_equal(fixture->history_items, 0);

  dt_remote_error_free(error);
  g_free(before);
  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_multiple_curves_produce_one_history_item(void **state)
{
  static const dt_remote_curve_point_t red[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.4, .y = 0.5 }, { .x = 1.0, .y = 1.0 },
  };
  static const dt_remote_curve_point_t green[] = {
    { .x = 0.0, .y = 0.05 }, { .x = 0.5, .y = 0.45 }, { .x = 1.0, .y = 0.95 },
  };
  static const dt_remote_curve_point_t blue[] = {
    { .x = 0.05, .y = 0.0 }, { .x = 0.6, .y = 0.7 }, { .x = 0.95, .y = 1.0 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  live_rgbcurve_enum_entry(fixture, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB"));
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.blue", blue, G_N_ELEMENTS(blue), TRUE,
                                            DT_REMOTE_CURVE_CATMULL_ROM));
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.red", red, G_N_ELEMENTS(red), TRUE,
                                            DT_REMOTE_CURVE_CUBIC_SPLINE));
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.green", green, G_N_ELEMENTS(green), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));

  dt_remote_error_t *error = NULL;
  assert_true(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_null(error);
  assert_int_equal(fixture->history_items, 1);
  assert_live_curve_points(fixture, "curve.red", red, G_N_ELEMENTS(red));
  assert_live_curve_points(fixture, "curve.green", green, G_N_ELEMENTS(green));
  assert_live_curve_points(fixture, "curve.blue", blue, G_N_ELEMENTS(blue));

  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_explicit_enable_participates_in_same_history_item(void **state)
{
  static const dt_remote_curve_point_t master[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.5, .y = 0.55 }, { .x = 1.0, .y = 1.0 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  fixture->module->enabled = FALSE;
  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.master", master, G_N_ELEMENTS(master), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  patch.has_enable = TRUE;
  patch.enable = TRUE;

  dt_remote_error_t *error = NULL;
  assert_true(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_null(error);
  assert_true(fixture->module->enabled);
  assert_int_equal(fixture->history_items, 1);

  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_stale_revision_leaves_state_unchanged(void **state)
{
  static const dt_remote_curve_point_t master[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.5, .y = 0.6 }, { .x = 1.0, .y = 1.0 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);
  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.master", master, G_N_ELEMENTS(master), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));

  dt_remote_error_t *error = NULL;
  assert_false(apply_scratch_transaction(fixture, &patch, TRUE, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_REVISION_CONFLICT);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_int_equal(fixture->history_items, 0);

  dt_remote_error_free(error);
  g_free(before);
  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_read_back_equals_live_semantic_state(void **state)
{
  static const dt_remote_curve_point_t master[] = {
    { .x = 0.08, .y = 0.11 }, { .x = 0.49, .y = 0.62 }, { .x = 0.96, .y = 0.9 },
  };
  live_rgbcurve_fixture_t *fixture = *state;
  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  live_rgbcurve_curve_patch("curve.master", master, G_N_ELEMENTS(master), TRUE,
                                            DT_REMOTE_CURVE_CATMULL_ROM));

  dt_remote_error_t *error = NULL;
  assert_true(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_null(error);
  assert_live_curve_points(fixture, "curve.master", master, G_N_ELEMENTS(master));
  GHashTable *values = NULL;
  assert_true(dt_remote_curve_read_values(fixture->module, fixture->module->params, &values, &error));
  assert_null(error);
  dt_remote_curve_value_t *live = g_hash_table_lookup(values, "curve.master");
  assert_int_equal(live->interpolation, DT_REMOTE_CURVE_CATMULL_ROM);

  g_hash_table_unref(values);
  live_rgbcurve_patch_cleanup(&patch);
}

static void test_transaction_middle_grey_without_work_profile_fails_without_mutation(void **state)
{
  live_rgbcurve_fixture_t *fixture = *state;
  assert_null(fixture->dev.full.pipe->work_profile_info);
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  dt_remote_patch_t patch;
  live_rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("compensate_middle_grey",
                             (dt_remote_value_t){ .type = DT_REMOTE_VALUE_BOOL, .v.b = TRUE }));

  dt_remote_error_t *error = NULL;
  assert_false(apply_scratch_transaction(fixture, &patch, FALSE, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  assert_non_null(error->details_json);
  assert_non_null(strstr(error->details_json, "\"constraint\":\"work_profile_unavailable\""));
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_int_equal(fixture->history_items, 0);

  dt_remote_error_free(error);
  g_free(before);
  live_rgbcurve_patch_cleanup(&patch);
}

static void test_schema_marks_denylisted_fields_unwritable(void **state)
{
  (void)state;
  // filmicrgb "version" passes the scalar type filter (it's an enum) but
  // must come back writable == FALSE and still be PRESENT in the schema.
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

/*
 * dt_remote_set_module_params()'s mutation-path denylist wiring: the
 * dt_remote_patch_apply() call site must pass dt_remote_denylist_for_op(
 * module->op), not NULL. An end-to-end call to dt_remote_set_module_params()
 * itself needs a live GUI darkroom -- dt_remote_require_darkroom_image()
 * gates on dt_view_get_current() == DT_VIEW_DARKROOM, and dt_view_get_current()
 * unconditionally returns DT_VIEW_LIGHTTABLE whenever darktable.view_manager
 * is NULL (src/views/view.c), which is exactly the state of this cmocka
 * harness's GUI-less dt_init(..., FALSE, FALSE, ...) -- there is no test
 * seam to fake past that gate, and inventing one is out of scope here.
 * That full path is instead exercised live, over the wire, by
 * tools/mcp/tests/integration/test_editing.py::
 * test_denylisted_fields_read_only_live.
 *
 * The closest regression guard reachable from this headless suite: the
 * exact (linear, denylist) composition the call site builds -- real
 * filmicrgb introspection plus the real per-op table via
 * dt_remote_denylist_for_op(), rather than the synthetic fixture_linear/
 * hand-built denylist the other dt_remote_patch_apply() tests above use --
 * still rejects a "version" entry whole, leaving the scratch params block
 * byte-identical to what it was.
 */
static void test_set_module_params_mutation_path_uses_real_denylist(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("filmicrgb");
  assert_non_null(so);
  dt_introspection_field_t *linear = so->get_introspection_linear();
  assert_non_null(linear);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *params = g_malloc0(intro->size);
  void *before = g_malloc(intro->size);
  memcpy(before, params, intro->size);

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("version", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_ENUM, .v.e = { 3, NULL } }));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(linear, dt_remote_denylist_for_op(so->op), &patch, params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  assert_memory_equal(params, before, intro->size);  // scratch block untouched on rejection
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
  g_free(params);
  g_free(before);
}

// The production wire promise for rgbcurve (the get_module_schema_rgbcurve
// response fixture): four semantic curves in registry order, and
// represented_by back-references from the three native storage fields to
// all four semantic IDs -- produced from the real adapter against the real
// introspection, so fixture and live server cannot drift apart silently.
static void test_schema_rgbcurve_semantic_fields_and_represented_by(void **state)
{
  (void)state;
  static const char *expected_ids[] = { "curve.master", "curve.red", "curve.green", "curve.blue" };

  dt_remote_module_schema_t *schema = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_get_module_schema("rgbcurve", &schema, &err));
  assert_null(err);
  assert_non_null(schema);
  assert_non_null(schema->semantic_fields);
  assert_int_equal(schema->semantic_fields->len, G_N_ELEMENTS(expected_ids));
  for(guint i = 0; i < G_N_ELEMENTS(expected_ids); i++)
  {
    const dt_remote_semantic_schema_t *wrapped = g_ptr_array_index(schema->semantic_fields, i);
    assert_int_equal(wrapped->class_id, DT_REMOTE_PARAMETER_CURVE);
    assert_string_equal(wrapped->u.curve->name, expected_ids[i]);
  }

  static const char *native_roots[] = { "curve_nodes", "curve_num_nodes", "curve_type" };
  for(guint r = 0; r < G_N_ELEMENTS(native_roots); r++)
  {
    const dt_remote_field_t *field = NULL;
    for(guint i = 0; i < schema->fields->len; i++)
    {
      const dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
      if(!g_strcmp0(f->name, native_roots[r])) { field = f; break; }
    }
    assert_non_null(field);
    assert_false(field->writable);  // native arrays stay primitive-unwritable
    assert_non_null(field->represented_by);
    assert_int_equal(field->represented_by->len, G_N_ELEMENTS(expected_ids));
    for(guint i = 0; i < G_N_ELEMENTS(expected_ids); i++)
      assert_string_equal(g_ptr_array_index(field->represented_by, i), expected_ids[i]);
  }

  // fields no curve routes through carry no represented_by
  for(guint i = 0; i < schema->fields->len; i++)
  {
    const dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
    if(!g_strcmp0(f->name, "curve_autoscale") || !g_strcmp0(f->name, "compensate_middle_grey"))
      assert_null(f->represented_by);
  }

  dt_remote_module_schema_free(schema);
}

static const dt_remote_curve_module_adapter_t *s_drift_adapter = NULL;

static const dt_remote_curve_module_adapter_t *drift_lookup_override(const char *operation,
                                                                     guint params_version)
{
  return !g_strcmp0(operation, "rgbcurve") && params_version == 1 ? s_drift_adapter : NULL;
}

// Registry validation is cached for the lifetime of the process by adapter
// identity. Mutation tests above correctly validate the production adapter,
// so this case uses an otherwise-identical copied adapter as a fresh cache
// identity while temporarily inducing native shape drift.
static void test_schema_rgbcurve_registry_drift_fails_closed(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);
  assert_non_null(intro->field);

  guint8 dummy_params = 0;
  dt_introspection_field_t *curve_nodes = NULL;
  assert_non_null(dt_introspection_get_child(intro->field, &dummy_params,
                                             "curve_nodes", &curve_nodes));
  assert_non_null(curve_nodes);
  assert_int_equal(curve_nodes->header.type, DT_INTROSPECTION_TYPE_ARRAY);
  assert_non_null(curve_nodes->Array.field);
  assert_int_equal(curve_nodes->Array.field->header.type, DT_INTROSPECTION_TYPE_ARRAY);

  dt_introspection_field_t *native_nodes = curve_nodes->Array.field;
  const size_t saved_count = native_nodes->Array.count;
  native_nodes->Array.count = 0;

  static dt_remote_curve_module_adapter_t drift_adapter;
  const dt_remote_curve_module_adapter_t *production_adapter =
    dt_remote_curve_registry_lookup("rgbcurve", (guint)intro->params_version);
  assert_non_null(production_adapter);
  drift_adapter = *production_adapter;
  s_drift_adapter = &drift_adapter;
  dt_remote_curve_registry_set_lookup_override(drift_lookup_override);

  dt_remote_module_schema_t *schema = NULL;
  dt_remote_error_t *err = NULL;
  const gboolean ok = dt_remote_get_module_schema("rgbcurve", &schema, &err);

  dt_remote_curve_registry_set_lookup_override(NULL);
  s_drift_adapter = NULL;
  native_nodes->Array.count = saved_count;

  dt_remote_module_schema_t *primitive_schema = NULL;
  dt_remote_error_t *primitive_err = NULL;
  const gboolean primitive_ok =
    dt_remote_get_module_primitive_schema("rgbcurve", &primitive_schema, &primitive_err);

  assert_false(ok);
  assert_null(schema);
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  assert_non_null(err->message);
  assert_true(strstr(err->message, "curve descriptor") != NULL);
  dt_remote_error_free(err);

  assert_true(primitive_ok);
  assert_non_null(primitive_schema);
  assert_non_null(primitive_schema->fields);
  assert_null(primitive_schema->semantic_fields);
  assert_null(primitive_err);
  dt_remote_module_schema_free(primitive_schema);
}

/* ---------------------------------------------------------------------- */
/* vector semantic seam tests (milestone 4, task 4)                        */
/* ---------------------------------------------------------------------- */

// "borders" test adapter: a single "color" vector over the real "color"
// float[3] field, subtype COLOR -- mirrors test_remote_vector.c's own
// borders fixture (the vector engine's own test harness), injected here
// through the same dt_remote_vector_registry_set_lookup_override() seam
// dt_remote_curve_registry_set_lookup_override() already established for
// curves, to prove the four remote_edit.c seams dispatch to the vector
// registry/engine exactly as they do to the curve one.
static const dt_remote_path_segment_t s_vector_color_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "color" },
};

static const dt_remote_vector_component_t s_vector_color_components[] = {
  { .name = "red", .minimum = 0.0, .maximum = 1.0 },
  { .name = "green", .minimum = 0.0, .maximum = 1.0 },
  { .name = "blue", .minimum = 0.0, .maximum = 1.0 },
};

static const dt_remote_vector_descriptor_t s_vector_color_descriptor = {
  .name = "color",
  .display_name = "Border color",
  .native = { .segments = s_vector_color_segments, .length = G_N_ELEMENTS(s_vector_color_segments) },
  .component_count = G_N_ELEMENTS(s_vector_color_components),
  .components = s_vector_color_components,
  .native_capacity = 3,
  .subtype = DT_REMOTE_VECTOR_COLOR,
  .color_space = "display_rgb",
};

static const dt_remote_vector_module_adapter_t s_vector_borders_adapter = {
  .operation = "borders",
  .minimum_params_version = 4,
  .maximum_params_version = 4,
  .vectors = &s_vector_color_descriptor,
  .vector_count = 1,
};

static const dt_remote_vector_module_adapter_t *vector_lookup_override(const char *operation,
                                                                        guint params_version)
{
  return !g_strcmp0(operation, s_vector_borders_adapter.operation)
         && params_version >= s_vector_borders_adapter.minimum_params_version
         && params_version <= s_vector_borders_adapter.maximum_params_version
    ? &s_vector_borders_adapter : NULL;
}

static const dt_remote_vector_module_adapter_t *always_null_vector_lookup(const char *operation,
                                                                           guint params_version)
{
  (void)operation;
  (void)params_version;
  return NULL;
}

// The schema seam (:892/:823 in the task brief): get_module_schema("borders")
// advertises the one injected vector alongside every untouched primitive
// field, and the annotation seam (_stamp_root/_annotate_represented_by)
// stamps the vector's semantic ID onto its native "color" field -- exactly
// the represented_by contract test_schema_rgbcurve_semantic_fields_and_
// represented_by proves for curves above.
static void test_schema_borders_vector_semantic_fields_and_represented_by(void **state)
{
  (void)state;
  dt_remote_vector_registry_set_lookup_override(vector_lookup_override);

  dt_remote_module_schema_t *schema = NULL;
  dt_remote_error_t *err = NULL;
  const gboolean ok = dt_remote_get_module_schema("borders", &schema, &err);

  dt_remote_vector_registry_set_lookup_override(NULL);

  assert_true(ok);
  assert_null(err);
  assert_non_null(schema);
  assert_non_null(schema->semantic_fields);
  assert_int_equal(schema->semantic_fields->len, 1);
  const dt_remote_semantic_schema_t *wrapped = g_ptr_array_index(schema->semantic_fields, 0);
  assert_int_equal(wrapped->class_id, DT_REMOTE_PARAMETER_VECTOR);
  assert_string_equal(wrapped->u.vector->name, "color");

  const dt_remote_field_t *color_field = NULL;
  gboolean found_size = FALSE;
  for(guint i = 0; i < schema->fields->len; i++)
  {
    const dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
    if(!g_strcmp0(f->name, "color")) color_field = f;
    // "size" is an untouched primitive float field -- present, writable,
    // and carrying no represented_by, exactly as it would without the
    // vector class ever existing.
    if(!g_strcmp0(f->name, "size"))
    {
      found_size = TRUE;
      assert_true(f->writable);
      assert_null(f->represented_by);
    }
  }
  assert_true(found_size);
  assert_non_null(color_field);
  assert_false(color_field->writable);  // native arrays stay primitive-unwritable
  assert_non_null(color_field->represented_by);
  assert_int_equal(color_field->represented_by->len, 1);
  assert_string_equal(g_ptr_array_index(color_field->represented_by, 0), "color");

  dt_remote_module_schema_free(schema);
}

/* --- borders scratch-transaction fixture --------------------------------- */

typedef struct live_borders_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} live_borders_fixture_t;

static live_borders_fixture_t *live_borders_fixture_new(void)
{
  live_borders_fixture_t *fixture = g_new0(live_borders_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;

  fixture->module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  assert_false(dt_iop_load_module(fixture->module, so, &fixture->dev));
  memcpy(fixture->module->params, fixture->module->default_params, fixture->module->params_size);
  fixture->dev.iop = g_list_append(fixture->dev.iop, fixture->module);
  return fixture;
}

static void live_borders_fixture_free(live_borders_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static int live_borders_test_setup(void **state)
{
  *state = live_borders_fixture_new();
  return 0;
}

static int live_borders_test_teardown(void **state)
{
  live_borders_fixture_free(*state);
  *state = NULL;
  return 0;
}

static dt_remote_semantic_patch_t *live_borders_vector_patch(const char *name, const double *values,
                                                              guint count)
{
  dt_remote_semantic_patch_t *semantic = g_new0(dt_remote_semantic_patch_t, 1);
  semantic->class_id = DT_REMOTE_PARAMETER_VECTOR;
  semantic->value.vector.name = g_strdup(name);
  semantic->value.vector.values = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
  g_array_append_vals(semantic->value.vector.values, values, count);
  return semantic;
}

// Mirrors assert_live_curve_points -- the readback seam's underlying engine
// call (dt_remote_vector_read_values), the same one dt_remote_get_module_params
// composes and wraps.
static void assert_live_vector_values(live_borders_fixture_t *fixture, const char *name,
                                      const double *expected, guint count)
{
  dt_remote_vector_registry_set_lookup_override(vector_lookup_override);
  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &error));
  dt_remote_vector_registry_set_lookup_override(NULL);
  assert_null(error);
  dt_remote_vector_value_t *value = g_hash_table_lookup(values, name);
  assert_non_null(value);
  assert_int_equal(value->values->len, count);
  for(guint i = 0; i < count; i++)
    assert_float_equal(g_array_index(value->values, double, i), expected[i], 1e-6);
  g_hash_table_unref(values);
}

// Mirrors the apply seam's two-call sequence
// (dt_remote_patch_apply + dt_remote_vector_apply_patch against the same
// scratch block) plus its merged read-back verification pass -- the same
// "missing a written entry" contract dt_remote_set_module_params() enforces
// (remote_edit.c) -- reproduced here at the engine-primitive level, exactly
// as the curve suite's apply_scratch_transaction reproduces the curve half
// (dt_remote_set_module_params() itself needs a live GUI darkroom, see the
// denylist mutation-path test's comment above). `readback_override` lets a
// caller simulate registry drift between the apply and read-back calls
// (test below); passing the real vector_lookup_override models the normal,
// non-drifted transaction.
static gboolean apply_vector_patch_with_readback_verification(
  live_borders_fixture_t *fixture,
  const dt_remote_patch_t *patch,
  dt_remote_vector_registry_lookup_override_t readback_override,
  dt_remote_error_t **error)
{
  dt_remote_vector_registry_set_lookup_override(vector_lookup_override);

  void *projected = g_malloc(fixture->module->params_size);
  memcpy(projected, fixture->module->params, fixture->module->params_size);

  dt_introspection_field_t *linear = fixture->module->so->get_introspection_linear();
  if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op("borders"), patch, projected, error))
  {
    g_free(projected);
    dt_remote_vector_registry_set_lookup_override(NULL);
    return FALSE;
  }

  if(!dt_remote_vector_apply_patch(fixture->module, fixture->module->params, projected, patch, error))
  {
    g_free(projected);
    dt_remote_vector_registry_set_lookup_override(NULL);
    return FALSE;
  }

  dt_remote_vector_registry_set_lookup_override(readback_override);
  GHashTable *readback = NULL;
  const gboolean read_ok = dt_remote_vector_read_values(fixture->module, projected, &readback, error);
  dt_remote_vector_registry_set_lookup_override(NULL);

  if(!read_ok)
  {
    g_free(projected);
    return FALSE;
  }

  // Restrict to exactly the semantic IDs this patch wrote -- the same rule
  // dt_remote_set_module_params()'s merged filter loop applies.
  guint written = 0;
  for(guint i = 0; i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(semantic->class_id == DT_REMOTE_PARAMETER_VECTOR
       && g_hash_table_lookup(readback, semantic->value.vector.name))
      written++;
  }
  const gboolean matched = written == patch->semantic_values->len;
  g_hash_table_unref(readback);

  if(!matched)
  {
    g_free(projected);
    if(error)
    {
      *error = g_new0(dt_remote_error_t, 1);
      (*error)->code = DT_REMOTE_ERR_INTERNAL;
      (*error)->message = g_strdup("semantic read-back is missing a written entry");
    }
    return FALSE;
  }

  memcpy(fixture->module->params, projected, fixture->module->params_size);
  g_free(projected);
  return TRUE;
}

// The readback seam (:973): a live borders instance's "params read" (the
// same dt_remote_vector_read_values() call dt_remote_get_module_params()
// composes) returns the vector value for the default, unwritten state.
static void test_params_read_borders_returns_vector_value(void **state)
{
  live_borders_fixture_t *fixture = *state;
  static const double default_color[] = { 1.0, 1.0, 1.0 };  // $DEFAULT: 1.0
  assert_live_vector_values(fixture, "color", default_color, G_N_ELEMENTS(default_color));
}

// The apply seam (:1070/:1085): a full patch -- one scalar field plus one
// vector semantic entry -- applies in the same scratch transaction and
// reads back the written vector value.
static void test_transaction_vector_patch_applies_and_reads_back(void **state)
{
  live_borders_fixture_t *fixture = *state;

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values,
                  make_entry("size", (dt_remote_value_t){ .type = DT_REMOTE_VALUE_FLOAT, .v.f = 0.2 }));
  patch.semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
  static const double new_color[] = { 0.25, 0.5, 0.75 };
  g_ptr_array_add(patch.semantic_values,
                  live_borders_vector_patch("color", new_color, G_N_ELEMENTS(new_color)));

  dt_remote_error_t *error = NULL;
  assert_true(
    apply_vector_patch_with_readback_verification(fixture, &patch, vector_lookup_override, &error));
  assert_null(error);
  assert_live_vector_values(fixture, "color", new_color, G_N_ELEMENTS(new_color));

  g_ptr_array_unref(patch.scalar_values);
  g_ptr_array_unref(patch.semantic_values);
}

// Simulated registry drift between the apply and read-back calls (the same
// drift class test_schema_rgbcurve_registry_drift_fails_closed proves for
// the schema seam): the read-back step observes no adapter at all, so the
// written "color" entry never comes back, and the whole transaction must
// fail closed with live params untouched.
static void test_transaction_vector_readback_mismatch_fails_mutation(void **state)
{
  live_borders_fixture_t *fixture = *state;

  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
  static const double new_color[] = { 0.2, 0.3, 0.4 };
  g_ptr_array_add(patch.semantic_values,
                  live_borders_vector_patch("color", new_color, G_N_ELEMENTS(new_color)));

  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  dt_remote_error_t *error = NULL;
  const gboolean ok = apply_vector_patch_with_readback_verification(fixture, &patch,
                                                                    always_null_vector_lookup, &error);

  assert_false(ok);
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_true(strstr(error->message, "missing a written entry") != NULL);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(before);
  g_ptr_array_unref(patch.semantic_values);
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
    cmocka_unit_test(test_denylist_for_op_lookup),
    cmocka_unit_test(test_schema_marks_denylisted_fields_unwritable),
    cmocka_unit_test(test_set_module_params_mutation_path_uses_real_denylist),

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

    cmocka_unit_test_setup_teardown(test_transaction_scalar_plus_curve_patch_commits_once,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_any_invalid_scalar_curve_leaves_every_field_unchanged,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_multiple_curves_produce_one_history_item,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_explicit_enable_participates_in_same_history_item,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_stale_revision_leaves_state_unchanged,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_read_back_equals_live_semantic_state,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_middle_grey_without_work_profile_fails_without_mutation,
                                   live_rgbcurve_test_setup, live_rgbcurve_test_teardown),

    cmocka_unit_test(test_schema_borders_vector_semantic_fields_and_represented_by),
    cmocka_unit_test_setup_teardown(test_params_read_borders_returns_vector_value,
                                   live_borders_test_setup, live_borders_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_vector_patch_applies_and_reads_back,
                                   live_borders_test_setup, live_borders_test_teardown),
    cmocka_unit_test_setup_teardown(test_transaction_vector_readback_mismatch_fails_mutation,
                                   live_borders_test_setup, live_borders_test_teardown),

    // Keep last: the case temporarily mutates real introspection, restoring
    // it immediately after the schema call.
    cmocka_unit_test(test_schema_rgbcurve_semantic_fields_and_represented_by),
    cmocka_unit_test(test_schema_rgbcurve_registry_drift_fails_closed),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
