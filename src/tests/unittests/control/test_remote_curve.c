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
 * cmocka unit tests for the neutral semantic-parameter types
 * (src/control/remote_parameters.c/.h) and the dt_remote_patch_t
 * extension in src/control/remote_edit.h/.c.
 *
 * This is pure data plus paired constructors/destructors -- no
 * introspection cursor, no registry, no JSON (those are later steps).
 * Coverage here:
 *
 *  - dt_remote_curve_schema_free(), dt_remote_curve_value_free(), and
 *    dt_remote_semantic_patch_free() on fully-populated, partially-
 *    populated (nullable members left NULL), and NULL instances -- every
 *    free must be a safe no-op on NULL, matching the idiom already used
 *    by dt_remote_patch_entry_free()/dt_remote_module_schema_free() in
 *    remote_edit.c.
 *  - the dt_remote_patch_apply() emptiness gate: a patch is empty (and
 *    rejected) only when scalar_values, semantic_values, and has_enable
 *    are all unset; semantic-only and enable-only patches now count as
 *    non-empty, while the scalar-only behavior already covered by
 *    test_remote_edit.c is unchanged.
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_edit.h"
#include "control/remote_parameters.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* A NONE-terminated introspection array with no leaves: sufficient for
 * dt_remote_patch_apply() gating tests below, which never populate
 * scalar_values and so never walk it. */
static const dt_introspection_field_t empty_linear[] = {
  { .header = { .type = DT_INTROSPECTION_TYPE_NONE } },
};

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_schema_free                                             */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *make_condition(const char *field, const char *enum_name)
{
  dt_remote_parameter_condition_t *c = g_new0(dt_remote_parameter_condition_t, 1);
  c->field = g_strdup(field);
  c->op = DT_REMOTE_PREDICATE_EQ;
  c->enum_name = g_strdup(enum_name);
  return c;
}

static void test_curve_schema_free_fully_populated(void **state)
{
  dt_remote_curve_schema_t *schema = g_new0(dt_remote_curve_schema_t, 1);
  schema->name = g_strdup("curve.master");
  schema->display_name = g_strdup("Master curve");
  schema->description = g_strdup("the master tone curve");
  schema->x.minimum = 0.0;
  schema->x.maximum = 1.0;
  schema->x.unit = g_strdup("normalized");
  schema->y.minimum = 0.0;
  schema->y.maximum = 1.0;
  schema->y.unit = g_strdup("normalized");
  schema->minimum_points = 2;
  schema->maximum_points = 20;
  schema->minimum_x_spacing = 0.001;
  schema->adjacent_spacing_rule = DT_REMOTE_SPACING_AT_LEAST;
  schema->minimum_wrap_spacing = 0.0;
  schema->wrap_spacing_rule = DT_REMOTE_SPACING_NONE;
  schema->strict_x_order = TRUE;
  schema->periodic_x = FALSE;
  schema->boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL;
  schema->interpolation_mask = 1u << DT_REMOTE_CURVE_CUBIC_SPLINE;
  schema->default_interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
  schema->writability = DT_REMOTE_WRITABLE_CONDITIONAL;
  schema->active_when = make_condition("mode", "MODE_RGB");
  schema->writable_when = make_condition("mode", "MODE_RGB");
  schema->periodic_when = make_condition("wrap", "WRAP_ON");

  dt_remote_curve_schema_free(schema);
  // no crash/double-free is the assertion; cmocka has nothing else to check
}

static void test_curve_schema_free_partially_populated(void **state)
{
  // description and the three conditions are documented nullable; leave
  // them unset to exercise the NULL branches inside the free function.
  dt_remote_curve_schema_t *schema = g_new0(dt_remote_curve_schema_t, 1);
  schema->name = g_strdup("curve.master");
  schema->display_name = g_strdup("Master curve");
  schema->writability = DT_REMOTE_WRITABLE_NOW;

  dt_remote_curve_schema_free(schema);
}

static void test_curve_schema_free_null_is_safe(void **state)
{
  dt_remote_curve_schema_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_value_free                                              */
/* ---------------------------------------------------------------------- */

static GArray *make_points(void)
{
  GArray *points = g_array_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t));
  dt_remote_curve_point_t p0 = { 0.0, 0.0 };
  dt_remote_curve_point_t p1 = { 1.0, 1.0 };
  g_array_append_val(points, p0);
  g_array_append_val(points, p1);
  return points;
}

static void test_curve_value_free_fully_populated(void **state)
{
  dt_remote_curve_value_t *value = g_new0(dt_remote_curve_value_t, 1);
  value->name = g_strdup("curve.master");
  value->points = make_points();
  value->interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE;
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;
  value->periodic_x = FALSE;

  dt_remote_curve_value_free(value);
}

static void test_curve_value_free_partially_populated(void **state)
{
  // points is documented owned but nothing else says it can't be empty at
  // construction time in some caller path; exercise points == NULL too.
  dt_remote_curve_value_t *value = g_new0(dt_remote_curve_value_t, 1);
  value->name = g_strdup("curve.master");

  dt_remote_curve_value_free(value);
}

static void test_curve_value_free_null_is_safe(void **state)
{
  dt_remote_curve_value_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_semantic_patch_free                                           */
/* ---------------------------------------------------------------------- */

static void test_semantic_patch_free_curve_fully_populated(void **state)
{
  dt_remote_semantic_patch_t *patch = g_new0(dt_remote_semantic_patch_t, 1);
  patch->class_id = DT_REMOTE_PARAMETER_CURVE;
  patch->value.curve.name = g_strdup("curve.master");
  patch->value.curve.points = make_points();
  patch->value.curve.has_interpolation = TRUE;
  patch->value.curve.interpolation = DT_REMOTE_CURVE_CATMULL_ROM;

  dt_remote_semantic_patch_free(patch);
}

static void test_semantic_patch_free_curve_partially_populated(void **state)
{
  // has_interpolation left FALSE and points left NULL: a patch that only
  // renames/moves points is not modeled here, but the free path must
  // still tolerate an unset points array.
  dt_remote_semantic_patch_t *patch = g_new0(dt_remote_semantic_patch_t, 1);
  patch->class_id = DT_REMOTE_PARAMETER_CURVE;
  patch->value.curve.name = g_strdup("curve.master");

  dt_remote_semantic_patch_free(patch);
}

static void test_semantic_patch_free_non_curve_class_is_safe(void **state)
{
  // DT_REMOTE_PARAMETER_SAMPLED_RESPONSE/LEVELS have no union member yet;
  // freeing a zeroed instance of either must not crash.
  dt_remote_semantic_patch_t *sampled = g_new0(dt_remote_semantic_patch_t, 1);
  sampled->class_id = DT_REMOTE_PARAMETER_SAMPLED_RESPONSE;
  dt_remote_semantic_patch_free(sampled);

  dt_remote_semantic_patch_t *levels = g_new0(dt_remote_semantic_patch_t, 1);
  levels->class_id = DT_REMOTE_PARAMETER_LEVELS;
  dt_remote_semantic_patch_free(levels);
}

static void test_semantic_patch_free_null_is_safe(void **state)
{
  dt_remote_semantic_patch_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_patch_apply emptiness gate                                    */
/* ---------------------------------------------------------------------- */

static GPtrArray *make_semantic_values_with_one_curve(void)
{
  GPtrArray *semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
  dt_remote_semantic_patch_t *entry = g_new0(dt_remote_semantic_patch_t, 1);
  entry->class_id = DT_REMOTE_PARAMETER_CURVE;
  entry->value.curve.name = g_strdup("curve.master");
  entry->value.curve.points = make_points();
  g_ptr_array_add(semantic_values, entry);
  return semantic_values;
}

typedef struct empty_params_t
{
  int unused;
} empty_params_t;

static void test_patch_apply_all_empty_rejected(void **state)
{
  empty_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };  // scalar_values NULL, semantic_values NULL, has_enable FALSE

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(empty_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);
}

static void test_patch_apply_semantic_only_patch_is_not_empty(void **state)
{
  empty_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = make_semantic_values_with_one_curve();

  dt_remote_error_t *err = NULL;
  // scalar_values is NULL, but the patch still has a semantic value, so
  // the "values must be non-empty" gate must not fire; nothing consumes
  // semantic_values yet (that is Task 8), so the call otherwise succeeds
  // trivially (no scalar entries to validate/write).
  assert_true(dt_remote_patch_apply(empty_linear, NULL, &patch, &params, &err));
  assert_null(err);

  g_ptr_array_unref(patch.semantic_values);
}

static void test_patch_apply_enable_only_patch_is_not_empty(void **state)
{
  empty_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.has_enable = TRUE;
  patch.enable = TRUE;

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_patch_apply(empty_linear, NULL, &patch, &params, &err));
  assert_null(err);
}

static void test_patch_apply_null_semantic_values_scalar_only_unchanged(void **state)
{
  // Regression guard: a scalar-only patch (semantic_values == NULL, as
  // every pre-Task-3 caller still constructs it) that is itself empty is
  // still rejected -- unchanged from before this task's gate fix.
  empty_params_t params = { 0 };
  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_patch_apply(empty_linear, NULL, &patch, &params, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  dt_remote_error_free(err);

  g_ptr_array_unref(patch.scalar_values);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_curve_schema_free_fully_populated),
    cmocka_unit_test(test_curve_schema_free_partially_populated),
    cmocka_unit_test(test_curve_schema_free_null_is_safe),
    cmocka_unit_test(test_curve_value_free_fully_populated),
    cmocka_unit_test(test_curve_value_free_partially_populated),
    cmocka_unit_test(test_curve_value_free_null_is_safe),
    cmocka_unit_test(test_semantic_patch_free_curve_fully_populated),
    cmocka_unit_test(test_semantic_patch_free_curve_partially_populated),
    cmocka_unit_test(test_semantic_patch_free_non_curve_class_is_safe),
    cmocka_unit_test(test_semantic_patch_free_null_is_safe),
    cmocka_unit_test(test_patch_apply_all_empty_rejected),
    cmocka_unit_test(test_patch_apply_semantic_only_patch_is_not_empty),
    cmocka_unit_test(test_patch_apply_enable_only_patch_is_not_empty),
    cmocka_unit_test(test_patch_apply_null_semantic_values_scalar_only_unchanged),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
