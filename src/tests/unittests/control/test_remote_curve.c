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
 * (src/control/remote_parameters.c/.h), the dt_remote_patch_t extension
 * in src/control/remote_edit.h/.c, and the bounds-checked introspection
 * path cursor (src/control/remote_curve.c/.h).
 *
 * The semantic-parameter/patch types are pure data plus paired
 * constructors/destructors -- no introspection, no registry, no JSON
 * (those are later steps). The path cursor is the first piece that does
 * touch introspection, walking a compiled-in path description against a
 * loaded module's introspection tree and a live params blob. Coverage
 * here:
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
 *  - dt_remote_path_resolve() against the real rgbcurve module .so:
 *    nested field/array-index sequences resolve to the right offset in
 *    a fixture params blob (curve_nodes[0][3].x, curve_num_nodes[1]),
 *    and wrong-type segments, out-of-bounds indices, and missing
 *    children are all rejected with DT_REMOTE_ERR_INTERNAL. rgbcurve's
 *    curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS][DT_IOP_RGBCURVE_MAXNODES]
 *    is a native two-dimensional C array; per tools/introspection/ast.pm
 *    ast_type_node::get_introspection_code() (each array dimension emits
 *    its own DT_INTROSPECTION_TYPE_ARRAY layer, innermost first) this is
 *    ARRAY-of-ARRAY-of-STRUCT in the introspection tree, so the path
 *    needs two DT_REMOTE_PATH_INDEX segments in a row, not one.
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
#include <json-glib/json-glib.h>

#include "../util/assert.h"

#include "control/remote_curve.h"
#include "control/remote_edit.h"
#include "control/remote_parameters.h"

#include "common/colorspaces.h"
#include "common/iop_profile.h"
#include "develop/develop.h"
#include "develop/imageop.h" // dt_iop_get_module_so()/dt_iop_module_so_t
#include "develop/pixelpipe.h"

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
  // the "values must be non-empty" gate must not fire. Semantic content is
  // consumed later by the curve engine; this scalar-only helper therefore
  // succeeds trivially (no scalar entries to validate/write).
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

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_validate                                                */
/* ---------------------------------------------------------------------- */

/*
 * dt_remote_curve_validate() is pure (no introspection, no registry, no
 * JSON parsing of the input), so these tests build dt_remote_curve_descriptor_t
 * instances by hand -- no live module or dt_init() dependency, unlike the
 * dt_remote_path_resolve() tests below. Coverage follows the curve-classes
 * design doc's "Validation algorithm" items 3-10 (the only items this
 * pure helper implements; the apply engine composes items 1-2/11-14 --
 * see the header comment on dt_remote_curve_validate()).
 */

static GArray *make_curve_points_xy(const double *xs, const double *ys, guint n)
{
  GArray *points = g_array_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t));
  for(guint i = 0; i < n; i++)
  {
    dt_remote_curve_point_t p = { xs[i], ys[i] };
    g_array_append_val(points, p);
  }
  return points;
}

// Parses err->details_json and asserts its "parameter"/"constraint" members,
// and its "point_index" member iff expected_point_index >= 0 (a negative
// expected value asserts the member is ABSENT -- the documented convention
// for whole-array failures like min_points/max_points and for
// interpolation_not_allowed).
static void assert_curve_error_details(dt_remote_error_t *err, const char *expected_parameter,
                                       int expected_point_index, const char *expected_constraint)
{
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(err->details_json);

  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, err->details_json, -1, NULL));
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
  assert_non_null(obj);

  assert_true(json_object_has_member(obj, "parameter"));
  assert_string_equal(json_object_get_string_member(obj, "parameter"), expected_parameter);

  assert_true(json_object_has_member(obj, "constraint"));
  assert_string_equal(json_object_get_string_member(obj, "constraint"), expected_constraint);

  if(expected_point_index >= 0)
  {
    assert_true(json_object_has_member(obj, "point_index"));
    assert_int_equal((gint64)json_object_get_int_member(obj, "point_index"), expected_point_index);
  }
  else
  {
    assert_false(json_object_has_member(obj, "point_index"));
  }

  g_object_unref(parser);
}

// rgbcurve-shaped descriptor: 2-20 points, spacing 0.0025 strictly greater,
// strict ascending order, optional boundary points, all three
// interpolations allowed. Matches Step 1's hand-built descriptor.
static dt_remote_curve_descriptor_t make_master_curve_descriptor(void)
{
  dt_remote_curve_descriptor_t desc = { 0 };
  desc.name = "curve.master";
  desc.x.minimum = 0.0;
  desc.x.maximum = 1.0;
  desc.x.unit = "normalized";
  desc.y.minimum = 0.0;
  desc.y.maximum = 1.0;
  desc.y.unit = "normalized";
  desc.minimum_points = 2;
  desc.maximum_points = 20;
  desc.minimum_x_spacing = 0.0025;
  desc.adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN;
  desc.minimum_wrap_spacing = 0.0;
  desc.wrap_spacing_rule = DT_REMOTE_SPACING_NONE;
  desc.strict_x_order = TRUE;
  desc.boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL;
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE)
                          | (1u << DT_REMOTE_CURVE_CATMULL_ROM)
                          | (1u << DT_REMOTE_CURVE_MONOTONE_HERMITE);
  desc.default_interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
  return desc;
}

static void test_curve_validate_accepts_well_formed_points(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 0.5, 0.9 };
  double ys[] = { 0.2, 0.6, 0.3 };
  GArray *points = make_curve_points_xy(xs, ys, 3);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, TRUE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

/* -- item 3: point count -- */

static void test_curve_validate_rejects_too_few_points(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.5 };
  double ys[] = { 0.5 };
  GArray *points = make_curve_points_xy(xs, ys, 1);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", -1, "min_points");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_too_many_points(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[21], ys[21];
  for(int i = 0; i < 21; i++)
  {
    xs[i] = i * 0.045; // well inside [0,1], well beyond min spacing
    ys[i] = 0.5;
  }
  GArray *points = make_curve_points_xy(xs, ys, 21);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", -1, "max_points");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

/* -- item 4: non-finite / domain -- */

static void test_curve_validate_rejects_nan(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, NAN };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "non_finite");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_positive_infinity(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, INFINITY };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "non_finite");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_x_outside_domain(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 1.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "domain_x");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_y_outside_domain(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, -0.3 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "domain_y");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

// no rejection here must ever touch the caller's points array.
static void test_curve_validate_never_mutates_points_on_rejection(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 1.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);
  GArray *snapshot = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_non_null(err);

  assert_int_equal(points->len, snapshot->len);
  for(guint i = 0; i < points->len; i++)
  {
    dt_remote_curve_point_t *a = &g_array_index(points, dt_remote_curve_point_t, i);
    dt_remote_curve_point_t *b = &g_array_index(snapshot, dt_remote_curve_point_t, i);
    assert_true(a->x == b->x);
    assert_true(a->y == b->y);
  }

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
  g_array_free(snapshot, TRUE);
}

/* -- item 5: strict ascending x -- */

static void test_curve_validate_rejects_descending_x(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.5, 0.3 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "strict_order");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_duplicate_x(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.5, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "duplicate_x");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

/* -- item 6: adjacent minimum spacing -- */

static void test_curve_validate_rejects_spacing_exactly_at_threshold_greater_than(void **state)
{
  (void)state;
  // GREATER_THAN is strict: a delta bit-identical to minimum_x_spacing
  // (both come from the same 0.0025 literal, so subtraction from 0.0 is
  // exact) must be rejected, not accepted.
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.0, 0.0025 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "curve.master", 1, "adjacent_spacing");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_accepts_spacing_just_above_threshold(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.0, 0.00251 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static dt_remote_curve_descriptor_t make_at_least_spacing_descriptor(void)
{
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.name = "test.at_least_spacing";
  desc.adjacent_spacing_rule = DT_REMOTE_SPACING_AT_LEAST;
  desc.minimum_x_spacing = 0.01;
  return desc;
}

static void test_curve_validate_accepts_spacing_exactly_at_threshold_at_least(void **state)
{
  (void)state;
  // AT_LEAST is inclusive: a delta bit-identical to minimum_x_spacing must
  // be accepted (the opposite of the GREATER_THAN case above).
  dt_remote_curve_descriptor_t desc = make_at_least_spacing_descriptor();
  double xs[] = { 0.0, 0.01 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_spacing_below_threshold_at_least(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_at_least_spacing_descriptor();
  double xs[] = { 0.0, 0.005 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.at_least_spacing", 1, "adjacent_spacing");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_spacing_none_skips_adjacent_check(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.adjacent_spacing_rule = DT_REMOTE_SPACING_NONE;
  // delta 0.0001 is far below the descriptor's own (now-ignored) 0.0025
  // minimum_x_spacing; passing proves the check is skipped, not just lax.
  double xs[] = { 0.1, 0.1001 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

/* -- item 7: periodic wrap spacing -- */

// Synthetic hue-channel-like descriptor: domain [0,1], periodic wrap
// spacing enabled (rgbcurve does not use this -- wrap_spacing_rule is
// DT_REMOTE_SPACING_NONE on the master descriptor above).
static dt_remote_curve_descriptor_t make_periodic_hue_descriptor(void)
{
  dt_remote_curve_descriptor_t desc = { 0 };
  desc.name = "test.hue";
  desc.x.minimum = 0.0;
  desc.x.maximum = 1.0;
  desc.y.minimum = 0.0;
  desc.y.maximum = 1.0;
  desc.minimum_points = 2;
  desc.maximum_points = 10;
  desc.adjacent_spacing_rule = DT_REMOTE_SPACING_NONE;
  desc.wrap_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN;
  desc.minimum_wrap_spacing = 0.01;
  desc.strict_x_order = TRUE;
  desc.boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL;
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE);
  desc.default_interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
  return desc;
}

static void test_curve_validate_rejects_wrap_spacing_below_threshold(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_periodic_hue_descriptor();
  // wrap gap = (first.x - x.min) + (x.max - last.x) = (0 - 0) + (1 - 0.995) = 0.005 < 0.01
  double xs[] = { 0.0, 0.995 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.hue", 0, "wrap_spacing");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_accepts_wrap_spacing_above_threshold(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_periodic_hue_descriptor();
  // wrap gap = (0 - 0) + (1 - 0.98) = 0.02 > 0.01
  double xs[] = { 0.0, 0.98 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static void test_curve_validate_wrap_spacing_none_skips_wrap_check(void **state)
{
  (void)state;
  // The master (rgbcurve) descriptor's wrap_spacing_rule is NONE; points
  // sitting right at both domain edges (wrap gap 0, far below what a
  // GREATER_THAN 0.0025 wrap rule would demand) must still pass.
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.0, 1.0 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

/* -- item 8: domain-boundary point policy -- */

static void test_curve_validate_optional_boundary_accepts_interior_endpoints(void **state)
{
  (void)state;
  // Already exercised by test_curve_validate_accepts_well_formed_points
  // (first.x=0.1, last.x=0.9, neither touching the domain edges), but
  // named explicitly here to document the OPTIONAL policy's contract.
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.2, 0.8 };
  double ys[] = { 0.3, 0.4 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static dt_remote_curve_descriptor_t make_required_boundary_descriptor(void)
{
  dt_remote_curve_descriptor_t desc = { 0 };
  desc.name = "test.required_boundary";
  desc.x.minimum = 0.0;
  desc.x.maximum = 1.0;
  desc.y.minimum = 0.0;
  desc.y.maximum = 1.0;
  desc.minimum_points = 2;
  desc.maximum_points = 10;
  desc.adjacent_spacing_rule = DT_REMOTE_SPACING_NONE;
  desc.wrap_spacing_rule = DT_REMOTE_SPACING_NONE;
  desc.strict_x_order = TRUE;
  desc.boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_REQUIRED;
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE);
  desc.default_interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
  return desc;
}

static void test_curve_validate_required_boundary_accepts_domain_edges(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_required_boundary_descriptor();
  // y is unconstrained under REQUIRED -- only x must hit the domain edges.
  double xs[] = { 0.0, 1.0 };
  double ys[] = { 0.3, 0.7 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static void test_curve_validate_required_boundary_rejects_first_off_minimum(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_required_boundary_descriptor();
  double xs[] = { 0.1, 1.0 };
  double ys[] = { 0.3, 0.7 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.required_boundary", 0, "boundary_policy");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_required_boundary_rejects_last_off_maximum(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_required_boundary_descriptor();
  double xs[] = { 0.0, 0.9 };
  double ys[] = { 0.3, 0.7 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.required_boundary", 1, "boundary_policy");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static dt_remote_curve_descriptor_t make_fixed_identity_boundary_descriptor(void)
{
  dt_remote_curve_descriptor_t desc = make_required_boundary_descriptor();
  desc.name = "test.fixed_identity_boundary";
  desc.boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_FIXED_IDENTITY;
  return desc;
}

static void test_curve_validate_fixed_identity_accepts_identity_endpoints(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_fixed_identity_boundary_descriptor();
  double xs[] = { 0.0, 1.0 };
  double ys[] = { 0.0, 1.0 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static void test_curve_validate_fixed_identity_rejects_first_y_off_minimum(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_fixed_identity_boundary_descriptor();
  // first.x is correct (0.0 == x.minimum) but first.y is not y.minimum.
  double xs[] = { 0.0, 1.0 };
  double ys[] = { 0.2, 1.0 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.fixed_identity_boundary", 0, "boundary_policy");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_fixed_identity_rejects_last_y_off_maximum(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_fixed_identity_boundary_descriptor();
  double xs[] = { 0.0, 1.0 };
  double ys[] = { 0.0, 0.8 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_CUBIC_SPLINE, &err));
  assert_curve_error_details(err, "test.fixed_identity_boundary", 1, "boundary_policy");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

/* -- items 9-10: interpolation resolution / allowlist -- */

static void test_curve_validate_accepts_allowed_interpolation(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, TRUE, DT_REMOTE_CURVE_MONOTONE_HERMITE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_disallowed_interpolation(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE); // only cubic spline allowed
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, TRUE, DT_REMOTE_CURVE_CATMULL_ROM, &err));
  assert_curve_error_details(err, "curve.master", -1, "interpolation_not_allowed");

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
}

static void test_curve_validate_rejects_negative_interpolation_without_mutating_points(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.interpolation_mask = G_MAXUINT;
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);
  GArray *snapshot = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, TRUE,
                                        (dt_remote_curve_interpolation_t)-1, &err));
  assert_curve_error_details(err, "curve.master", -1, "interpolation_not_allowed");
  assert_int_equal(points->len, snapshot->len);
  assert_memory_equal(points->data, snapshot->data,
                      points->len * sizeof(dt_remote_curve_point_t));

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
  g_array_free(snapshot, TRUE);
}

static void test_curve_validate_rejects_oversized_interpolation_without_mutating_points(void **state)
{
  (void)state;
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE);
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);
  GArray *snapshot = make_curve_points_xy(xs, ys, 2);
  const dt_remote_curve_interpolation_t interpolation =
    (dt_remote_curve_interpolation_t)(sizeof(desc.interpolation_mask) * CHAR_BIT);

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_validate(&desc, points, TRUE, interpolation, &err));
  assert_curve_error_details(err, "curve.master", -1, "interpolation_not_allowed");
  assert_int_equal(points->len, snapshot->len);
  assert_memory_equal(points->data, snapshot->data,
                      points->len * sizeof(dt_remote_curve_point_t));

  dt_remote_error_free(err);
  g_array_free(points, TRUE);
  g_array_free(snapshot, TRUE);
}

static void test_curve_validate_omitted_interpolation_skips_allowlist_check(void **state)
{
  (void)state;
  // has_interpolation == FALSE: resolving/preserving the current
  // interpolation is the apply engine's job (item 9's other half), so no
  // allowlist check runs here at all -- passing an otherwise-disallowed
  // value in `interpolation` must not matter.
  dt_remote_curve_descriptor_t desc = make_master_curve_descriptor();
  desc.interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE); // only cubic spline allowed
  double xs[] = { 0.1, 0.5 };
  double ys[] = { 0.2, 0.6 };
  GArray *points = make_curve_points_xy(xs, ys, 2);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_validate(&desc, points, FALSE, DT_REMOTE_CURVE_MONOTONE_HERMITE, &err));
  assert_null(err);

  g_array_free(points, TRUE);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_path_resolve                                                  */
/* ---------------------------------------------------------------------- */

/*
 * dt_remote_path_resolve() needs a real loaded module .so: the path
 * segments are compiled-in registry data, but resolving them means
 * walking the *real* introspection tree generated for
 * dt_iop_rgbcurve_params_t (src/iop/rgbcurve.c) -- a type that, like
 * every other iop module's params struct, is private to that module's
 * .c file and only reachable through the dlopen'ed .so's introspection
 * API, never through a header the test could #include. So the "fixture
 * params blob" here is a heap buffer sized from the module's own
 * dt_introspection_t.size (documented as "size of the params struct"),
 * not a stack instance of the real C type -- the whole point of the
 * introspection cursor is that callers never need that type at compile
 * time. See harness_group_setup() below for the same minimal, GUI-less,
 * throwaway-configdir dt_init() test_remote_edit.c uses to populate
 * darktable.iop for its one real-module test.
 */

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_harness_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_harness_confdir = g_dir_make_tmp("test_remote_curve-XXXXXX", &gerror);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_curve: failed to create scratch config dir: %s\n",
            gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_curve",
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
      fprintf(stderr, "test_remote_curve: failed to remove scratch config dir %s\n",
              s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

/* ---------------------------------------------------------------------- */
/* rgbcurve mutation fixture                                               */
/* ---------------------------------------------------------------------- */

typedef struct rgbcurve_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} rgbcurve_fixture_t;

static rgbcurve_fixture_t *adapter_fixture_new(const char *op)
{
  rgbcurve_fixture_t *fixture = g_new0(rgbcurve_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);

  // Keep module construction headless even though the fixture owns the
  // three pipes a live darkroom develop would have. The adapter needs the
  // current full pipe for work-profile lookup, not module GUI widgets.
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

static void rgbcurve_fixture_free(rgbcurve_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static void *rgbcurve_field_ptr(const rgbcurve_fixture_t *fixture,
                                void *params,
                                const char *name,
                                dt_introspection_field_t **out_field)
{
  dt_introspection_t *intro = fixture->module->so->get_introspection();
  assert_non_null(intro);
  assert_non_null(intro->field);
  void *ptr = dt_introspection_get_child(intro->field, params, name, out_field);
  assert_non_null(ptr);
  assert_non_null(*out_field);
  return ptr;
}

static int rgbcurve_enum_value(const rgbcurve_fixture_t *fixture,
                               const char *field_name,
                               const char *enum_name)
{
  dt_introspection_field_t *field = NULL;
  (void)rgbcurve_field_ptr(fixture, fixture->module->params, field_name, &field);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_ENUM);
  int value = 0;
  assert_true(dt_introspection_get_enum_value(field, enum_name, &value));
  return value;
}

static void rgbcurve_set_enum(const rgbcurve_fixture_t *fixture,
                              void *params,
                              const char *field_name,
                              const char *enum_name)
{
  dt_introspection_field_t *field = NULL;
  int *ptr = rgbcurve_field_ptr(fixture, params, field_name, &field);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_ENUM);
  assert_true(dt_introspection_get_enum_value(field, enum_name, ptr));
}

static void rgbcurve_set_bool(const rgbcurve_fixture_t *fixture,
                              void *params,
                              const char *field_name,
                              gboolean value)
{
  dt_introspection_field_t *field = NULL;
  gboolean *ptr = rgbcurve_field_ptr(fixture, params, field_name, &field);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_BOOL);
  *ptr = value;
}

static void rgbcurve_write_integer_array(const rgbcurve_fixture_t *fixture,
                                         void *params,
                                         const char *field_name,
                                         guint index,
                                         int value)
{
  dt_introspection_field_t *array_field = NULL;
  void *array_ptr = rgbcurve_field_ptr(fixture, params, field_name, &array_field);
  dt_introspection_field_t *element_field = NULL;
  int *element_ptr = dt_introspection_access_array(array_field, array_ptr, index, &element_field);
  assert_non_null(element_ptr);
  assert_non_null(element_field);
  assert_int_equal(element_field->header.type, DT_INTROSPECTION_TYPE_INT);
  *element_ptr = value;
}

static int rgbcurve_read_integer_array(const rgbcurve_fixture_t *fixture,
                                       void *params,
                                       const char *field_name,
                                       guint index)
{
  dt_introspection_field_t *array_field = NULL;
  void *array_ptr = rgbcurve_field_ptr(fixture, params, field_name, &array_field);
  dt_introspection_field_t *element_field = NULL;
  int *element_ptr = dt_introspection_access_array(array_field, array_ptr, index, &element_field);
  assert_non_null(element_ptr);
  assert_non_null(element_field);
  assert_int_equal(element_field->header.type, DT_INTROSPECTION_TYPE_INT);
  return *element_ptr;
}

static void rgbcurve_write_native_point(const rgbcurve_fixture_t *fixture,
                                        void *params,
                                        guint channel,
                                        guint point,
                                        double x,
                                        double y)
{
  dt_introspection_field_t *channels_field = NULL;
  void *channels_ptr = rgbcurve_field_ptr(fixture, params, "curve_nodes", &channels_field);
  dt_introspection_field_t *nodes_field = NULL;
  void *nodes_ptr = dt_introspection_access_array(channels_field, channels_ptr, channel, &nodes_field);
  assert_non_null(nodes_ptr);
  assert_non_null(nodes_field);
  dt_introspection_field_t *node_field = NULL;
  void *node_ptr = dt_introspection_access_array(nodes_field, nodes_ptr, point, &node_field);
  assert_non_null(node_ptr);
  assert_non_null(node_field);

  dt_introspection_field_t *x_field = NULL;
  dt_introspection_field_t *y_field = NULL;
  float *x_ptr = dt_introspection_get_child(node_field, node_ptr, "x", &x_field);
  float *y_ptr = dt_introspection_get_child(node_field, node_ptr, "y", &y_field);
  assert_non_null(x_ptr);
  assert_non_null(y_ptr);
  assert_int_equal(x_field->header.type, DT_INTROSPECTION_TYPE_FLOAT);
  assert_int_equal(y_field->header.type, DT_INTROSPECTION_TYPE_FLOAT);
  *x_ptr = (float)x;
  *y_ptr = (float)y;
}

static dt_remote_curve_point_t rgbcurve_read_native_point(const rgbcurve_fixture_t *fixture,
                                                          void *params,
                                                          guint channel,
                                                          guint point)
{
  dt_introspection_field_t *channels_field = NULL;
  void *channels_ptr = rgbcurve_field_ptr(fixture, params, "curve_nodes", &channels_field);
  dt_introspection_field_t *nodes_field = NULL;
  void *nodes_ptr = dt_introspection_access_array(channels_field, channels_ptr, channel, &nodes_field);
  assert_non_null(nodes_ptr);
  assert_non_null(nodes_field);
  dt_introspection_field_t *node_field = NULL;
  void *node_ptr = dt_introspection_access_array(nodes_field, nodes_ptr, point, &node_field);
  assert_non_null(node_ptr);
  assert_non_null(node_field);

  dt_introspection_field_t *x_field = NULL;
  dt_introspection_field_t *y_field = NULL;
  float *x_ptr = dt_introspection_get_child(node_field, node_ptr, "x", &x_field);
  float *y_ptr = dt_introspection_get_child(node_field, node_ptr, "y", &y_field);
  assert_non_null(x_ptr);
  assert_non_null(y_ptr);
  return (dt_remote_curve_point_t){ .x = *x_ptr, .y = *y_ptr };
}

static void rgbcurve_set_native_curve(const rgbcurve_fixture_t *fixture,
                                      void *params,
                                      guint channel,
                                      const dt_remote_curve_point_t *points,
                                      guint count,
                                      int interpolation)
{
  for(guint i = 0; i < count; i++)
    rgbcurve_write_native_point(fixture, params, channel, i, points[i].x, points[i].y);
  rgbcurve_write_integer_array(fixture, params, "curve_num_nodes", channel, (int)count);
  rgbcurve_write_integer_array(fixture, params, "curve_type", channel, interpolation);
}

static dt_remote_semantic_patch_t *rgbcurve_make_curve_patch(
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

static dt_remote_patch_entry_t *rgbcurve_make_enum_entry(const rgbcurve_fixture_t *fixture,
                                                         const char *field_name,
                                                         const char *enum_name)
{
  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup(field_name);
  entry->value.type = DT_REMOTE_VALUE_ENUM;
  entry->value.v.e.value = rgbcurve_enum_value(fixture, field_name, enum_name);
  entry->value.v.e.name = g_strdup(enum_name);
  return entry;
}

static dt_remote_patch_entry_t *rgbcurve_make_bool_entry(const char *field_name, gboolean value)
{
  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup(field_name);
  entry->value.type = DT_REMOTE_VALUE_BOOL;
  entry->value.v.b = value;
  return entry;
}

static void rgbcurve_patch_init(dt_remote_patch_t *patch)
{
  memset(patch, 0, sizeof(*patch));
  patch->scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  patch->semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
}

static void rgbcurve_patch_cleanup(dt_remote_patch_t *patch)
{
  g_ptr_array_unref(patch->scalar_values);
  g_ptr_array_unref(patch->semantic_values);
}

static gboolean rgbcurve_apply_to_copy(const rgbcurve_fixture_t *fixture,
                                       const dt_remote_patch_t *patch,
                                       void *projected,
                                       dt_remote_error_t **error)
{
  memcpy(projected, fixture->module->params, fixture->module->params_size);
  dt_introspection_field_t *linear = fixture->module->so->get_introspection_linear();
  if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op(fixture->module->op), patch, projected, error))
    return FALSE;
  return dt_remote_curve_apply_patch(fixture->module, fixture->module->params,
                                     projected, patch, error);
}

static dt_remote_curve_value_t *rgbcurve_read_value(const rgbcurve_fixture_t *fixture,
                                                    void *params,
                                                    const char *name,
                                                    GHashTable **owned_values)
{
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_read_values(fixture->module, params, owned_values, &error));
  assert_null(error);
  dt_remote_curve_value_t *value = g_hash_table_lookup(*owned_values, name);
  assert_non_null(value);
  return value;
}

static void assert_curve_points(const dt_remote_curve_value_t *value,
                                const dt_remote_curve_point_t *expected,
                                guint count,
                                double epsilon)
{
  assert_int_equal(value->points->len, count);
  for(guint i = 0; i < count; i++)
  {
    const dt_remote_curve_point_t actual =
      g_array_index(value->points, dt_remote_curve_point_t, i);
    assert_float_equal(actual.x, expected[i].x, epsilon);
    assert_float_equal(actual.y, expected[i].y, epsilon);
  }
}

static void test_rgbcurve_adapter_linked_mode_exposes_only_curve_master(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");

  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_read_values(fixture->module, fixture->module->params, &values, &error));
  assert_null(error);
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.master"))->active);
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.master"))->writable_now);
  assert_false(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.red"))->active);
  assert_false(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.green"))->active);
  assert_false(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.blue"))->active);

  g_hash_table_unref(values);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_manual_mode_exposes_rgb_not_master(void **state)
{
  (void)state;
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB");

  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_curve_read_values(fixture->module, fixture->module->params, &values, &error));
  assert_null(error);
  assert_false(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.master"))->active);
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.red"))->active);
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.green"))->active);
  assert_true(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.blue"))->active);

  g_hash_table_unref(values);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_native_channel_zero_round_trips_under_correct_id(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t points[] = {
    { .x = 0.08, .y = 0.12 }, { .x = 0.47, .y = 0.61 }, { .x = 0.93, .y = 0.88 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");
  rgbcurve_write_integer_array(fixture, fixture->module->params, "curve_type", 0, 1);

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.master", points, G_N_ELEMENTS(points), FALSE,
                                            DT_REMOTE_CURVE_CUBIC_SPLINE));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *master = rgbcurve_read_value(fixture, projected, "curve.master", &values);
  assert_curve_points(master, points, G_N_ELEMENTS(points), 1e-6);
  assert_int_equal(master->interpolation, DT_REMOTE_CURVE_CATMULL_ROM);
  assert_false(((dt_remote_curve_value_t *)g_hash_table_lookup(values, "curve.red"))->active);

  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_all_interpolation_names_map_correctly(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t points[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.5, .y = 0.6 }, { .x = 1.0, .y = 1.0 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");

  for(dt_remote_curve_interpolation_t interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
      interpolation <= DT_REMOTE_CURVE_MONOTONE_HERMITE; interpolation++)
  {
    dt_remote_patch_t patch;
    rgbcurve_patch_init(&patch);
    g_ptr_array_add(patch.semantic_values,
                    rgbcurve_make_curve_patch("curve.master", points, G_N_ELEMENTS(points), TRUE,
                                              interpolation));
    void *projected = g_malloc(fixture->module->params_size);
    dt_remote_error_t *error = NULL;
    assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
    assert_null(error);

    GHashTable *values = NULL;
    dt_remote_curve_value_t *master = rgbcurve_read_value(fixture, projected, "curve.master", &values);
    assert_int_equal(master->interpolation, interpolation);
    assert_int_equal(rgbcurve_read_integer_array(fixture, projected, "curve_type", 0), interpolation);

    memcpy(fixture->module->params, projected, fixture->module->params_size);
    g_hash_table_unref(values);
    g_free(projected);
    rgbcurve_patch_cleanup(&patch);
  }

  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_mode_transition_multi_curve_patch_is_atomic(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t valid_red[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.4, .y = 0.5 }, { .x = 1.0, .y = 1.0 },
  };
  static const dt_remote_curve_point_t invalid_green[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 0.0025, .y = 0.2 }, { .x = 1.0, .y = 1.0 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  rgbcurve_make_enum_entry(fixture, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB"));
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.red", valid_red, G_N_ELEMENTS(valid_red), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.green", invalid_green, G_N_ELEMENTS(invalid_green), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_interior_endpoints_round_trip_without_normalization(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t points[] = {
    { .x = 0.07, .y = 0.16 }, { .x = 0.5, .y = 0.42 }, { .x = 0.94, .y = 0.89 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.master", points, G_N_ELEMENTS(points), TRUE,
                                            DT_REMOTE_CURVE_CUBIC_SPLINE));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *master = rgbcurve_read_value(fixture, projected, "curve.master", &values);
  assert_curve_points(master, points, G_N_ELEMENTS(points), 1e-6);

  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_entering_manual_copies_master_before_explicit_replacement(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t master_points[] = {
    { .x = 0.0, .y = 0.03 }, { .x = 0.45, .y = 0.57 }, { .x = 1.0, .y = 0.97 },
  };
  static const dt_remote_curve_point_t identity[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 1.0, .y = 1.0 },
  };
  static const dt_remote_curve_point_t green_replacement[] = {
    { .x = 0.1, .y = 0.2 }, { .x = 0.7, .y = 0.6 }, { .x = 0.95, .y = 0.9 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");
  rgbcurve_set_native_curve(fixture, fixture->module->params, 0, master_points,
                            G_N_ELEMENTS(master_points), DT_REMOTE_CURVE_CATMULL_ROM);
  rgbcurve_set_native_curve(fixture, fixture->module->params, 1, identity,
                            G_N_ELEMENTS(identity), DT_REMOTE_CURVE_CUBIC_SPLINE);
  rgbcurve_set_native_curve(fixture, fixture->module->params, 2, identity,
                            G_N_ELEMENTS(identity), DT_REMOTE_CURVE_CUBIC_SPLINE);

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  rgbcurve_make_enum_entry(fixture, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB"));
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.green", green_replacement,
                                            G_N_ELEMENTS(green_replacement), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *red = rgbcurve_read_value(fixture, projected, "curve.red", &values);
  dt_remote_curve_value_t *green = g_hash_table_lookup(values, "curve.green");
  dt_remote_curve_value_t *blue = g_hash_table_lookup(values, "curve.blue");
  assert_curve_points(red, master_points, G_N_ELEMENTS(master_points), 1e-6);
  assert_curve_points(green, green_replacement, G_N_ELEMENTS(green_replacement), 1e-6);
  assert_curve_points(blue, master_points, G_N_ELEMENTS(master_points), 1e-6);
  assert_int_equal(blue->interpolation, DT_REMOTE_CURVE_CATMULL_ROM);

  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_middle_grey_transforms_untouched_before_explicit_replacement(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t red_points[] = {
    { .x = 0.1, .y = 0.2 }, { .x = 0.8, .y = 0.7 },
  };
  static const dt_remote_curve_point_t green_points[] = {
    { .x = 0.2, .y = 0.3 }, { .x = 0.9, .y = 0.8 },
  };
  static const dt_remote_curve_point_t blue_points[] = {
    { .x = 0.15, .y = 0.25 }, { .x = 0.85, .y = 0.75 },
  };
  static const dt_remote_curve_point_t red_replacement[] = {
    { .x = 0.05, .y = 0.1 }, { .x = 0.95, .y = 0.9 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_MANUAL_RGB");
  rgbcurve_set_bool(fixture, fixture->module->params, "compensate_middle_grey", FALSE);
  rgbcurve_set_native_curve(fixture, fixture->module->params, 0, red_points, G_N_ELEMENTS(red_points), 2);
  rgbcurve_set_native_curve(fixture, fixture->module->params, 1, green_points, G_N_ELEMENTS(green_points), 2);
  rgbcurve_set_native_curve(fixture, fixture->module->params, 2, blue_points, G_N_ELEMENTS(blue_points), 2);

  const dt_iop_order_iccprofile_info_t *profile =
    dt_ioppr_set_pipe_work_profile_info(&fixture->dev, fixture->dev.full.pipe,
                                        DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_PERCEPTUAL);
  assert_non_null(profile);

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values, rgbcurve_make_bool_entry("compensate_middle_grey", TRUE));
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.red", red_replacement,
                                            G_N_ELEMENTS(red_replacement), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  GHashTable *values = NULL;
  dt_remote_curve_value_t *red = rgbcurve_read_value(fixture, projected, "curve.red", &values);
  dt_remote_curve_value_t *green = g_hash_table_lookup(values, "curve.green");
  dt_remote_curve_value_t *blue = g_hash_table_lookup(values, "curve.blue");
  assert_curve_points(red, red_replacement, G_N_ELEMENTS(red_replacement), 1e-6);
  for(guint i = 0; i < G_N_ELEMENTS(green_points); i++)
  {
    const dt_remote_curve_point_t actual =
      g_array_index(green->points, dt_remote_curve_point_t, i);
    assert_float_equal(actual.x, dt_ioppr_compensate_middle_grey(green_points[i].x, profile), 1e-5);
    assert_float_equal(actual.y, dt_ioppr_compensate_middle_grey(green_points[i].y, profile), 1e-5);
  }
  for(guint i = 0; i < G_N_ELEMENTS(blue_points); i++)
  {
    const dt_remote_curve_point_t actual =
      g_array_index(blue->points, dt_remote_curve_point_t, i);
    assert_float_equal(actual.x, dt_ioppr_compensate_middle_grey(blue_points[i].x, profile), 1e-5);
    assert_float_equal(actual.y, dt_ioppr_compensate_middle_grey(blue_points[i].y, profile), 1e-5);
  }

  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_invalid_green_blue_write_in_linked_mode_is_unsupported(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t points[] = {
    { .x = 0.0, .y = 0.0 }, { .x = 1.0, .y = 1.0 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");

  for(guint i = 0; i < 2; i++)
  {
    const char *name = i == 0 ? "curve.green" : "curve.blue";
    dt_remote_patch_t patch;
    rgbcurve_patch_init(&patch);
    g_ptr_array_add(patch.semantic_values,
                    rgbcurve_make_curve_patch(name, points, G_N_ELEMENTS(points), TRUE,
                                              DT_REMOTE_CURVE_MONOTONE_HERMITE));
    void *projected = g_malloc(fixture->module->params_size);
    dt_remote_error_t *error = NULL;
    assert_false(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
    assert_non_null(error);
    assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
    assert_non_null(error->details_json);
    assert_non_null(strstr(error->details_json, name));
    dt_remote_error_free(error);
    g_free(projected);
    rgbcurve_patch_cleanup(&patch);
  }

  rgbcurve_fixture_free(fixture);
}

static void test_rgbcurve_adapter_unused_native_capacity_is_zero_and_not_returned(void **state)
{
  (void)state;
  static const dt_remote_curve_point_t points[] = {
    { .x = 0.1, .y = 0.2 }, { .x = 0.9, .y = 0.8 },
  };
  rgbcurve_fixture_t *fixture = rgbcurve_fixture_new();
  rgbcurve_set_enum(fixture, fixture->module->params, "curve_autoscale", "DT_S_SCALE_AUTOMATIC_RGB");
  for(guint i = 0; i < 20; i++)
    rgbcurve_write_native_point(fixture, fixture->module->params, 0, i, 0.4, 0.6);
  rgbcurve_write_integer_array(fixture, fixture->module->params, "curve_num_nodes", 0, 20);

  dt_remote_patch_t patch;
  rgbcurve_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  rgbcurve_make_curve_patch("curve.master", points, G_N_ELEMENTS(points), TRUE,
                                            DT_REMOTE_CURVE_MONOTONE_HERMITE));
  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(rgbcurve_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  assert_int_equal(rgbcurve_read_integer_array(fixture, projected, "curve_num_nodes", 0), 2);
  for(guint i = 2; i < 20; i++)
  {
    const dt_remote_curve_point_t unused = rgbcurve_read_native_point(fixture, projected, 0, i);
    assert_float_equal(unused.x, 0.0, 0.0);
    assert_float_equal(unused.y, 0.0, 0.0);
  }
  GHashTable *values = NULL;
  dt_remote_curve_value_t *master = rgbcurve_read_value(fixture, projected, "curve.master", &values);
  assert_curve_points(master, points, G_N_ELEMENTS(points), 1e-6);

  g_hash_table_unref(values);
  g_free(projected);
  rgbcurve_patch_cleanup(&patch);
  rgbcurve_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* tonecurve adapter                                                       */
/* ---------------------------------------------------------------------- */

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

/* rgbcurve path segment helpers: curve_nodes is
 * dt_iop_rgbcurve_node_t[DT_IOP_RGBCURVE_MAX_CHANNELS][DT_IOP_RGBCURVE_MAXNODES],
 * a native two-dimensional C array. Per tools/introspection/ast.pm
 * ast_type_node::get_introspection_code(), each array dimension of a
 * declarator gets its own DT_INTROSPECTION_TYPE_ARRAY layer in the
 * introspection tree (innermost/element dimension first, then wrapping
 * outward) -- so this is ARRAY-of-ARRAY-of-STRUCT, and reaching a leaf
 * needs two DT_REMOTE_PATH_INDEX segments in a row, not one. */

static const dt_remote_path_segment_t curve_node_x_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },  // channel 0 (R)
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },  // node 3
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
};
static const dt_remote_introspection_path_t curve_node_x_path = {
  .segments = curve_node_x_segments,
  .length = G_N_ELEMENTS(curve_node_x_segments),
};

static const dt_remote_path_segment_t curve_num_nodes_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_num_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },  // channel 1 (G)
};
static const dt_remote_introspection_path_t curve_num_nodes_path = {
  .segments = curve_num_nodes_segments,
  .length = G_N_ELEMENTS(curve_num_nodes_segments),
};

// wrong-type segment: curve_nodes is an ARRAY field, not a STRUCT/UNION,
// so descending into it with a FIELD segment (instead of INDEX first)
// must fail dt_introspection_get_child()'s type check.
static const dt_remote_path_segment_t wrong_type_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
};
static const dt_remote_introspection_path_t wrong_type_path = {
  .segments = wrong_type_segments,
  .length = G_N_ELEMENTS(wrong_type_segments),
};

// out-of-bounds index: DT_IOP_RGBCURVE_MAXNODES is MAX_ANCHORS == 20, so
// node index 999 is out of bounds for the inner array.
static const dt_remote_path_segment_t out_of_bounds_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 999 },
};
static const dt_remote_introspection_path_t out_of_bounds_path = {
  .segments = out_of_bounds_segments,
  .length = G_N_ELEMENTS(out_of_bounds_segments),
};

// missing child: no such top-level field on dt_iop_rgbcurve_params_t.
static const dt_remote_path_segment_t missing_child_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "does_not_exist" },
};
static const dt_remote_introspection_path_t missing_child_path = {
  .segments = missing_child_segments,
  .length = G_N_ELEMENTS(missing_child_segments),
};

// type drift: curve_autoscale is an enum, not an array, so indexing it
// must fail dt_introspection_access_array()'s type check.
static const dt_remote_path_segment_t index_into_non_array_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_autoscale" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t index_into_non_array_path = {
  .segments = index_into_non_array_segments,
  .length = G_N_ELEMENTS(index_into_non_array_segments),
};

static void test_path_resolve_curve_node_x(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);
  assert_non_null(intro->field);
  assert_int_equal(intro->field->header.type, DT_INTROSPECTION_TYPE_STRUCT);

  void *fixture = g_malloc0(intro->size);

  const dt_introspection_field_t *out_field = NULL;
  void *out_ptr = NULL;
  dt_remote_error_t *err = NULL;

  assert_true(dt_remote_path_resolve(&curve_node_x_path, intro->field, fixture,
                                      &out_field, &out_ptr, &err));
  assert_null(err);
  assert_non_null(out_field);
  assert_non_null(out_ptr);
  assert_int_equal(out_field->header.type, DT_INTROSPECTION_TYPE_FLOAT);

  // the resolved pointer must land strictly inside the fixture blob.
  ptrdiff_t offset = (char *)out_ptr - (char *)fixture;
  assert_true(offset >= 0);
  assert_true((size_t)offset + out_field->header.size <= intro->size);

  // write through the resolved pointer, then resolve the identical path
  // again against the same fixture: the cursor must be deterministic
  // (same offset every time) and the value read back must be exactly
  // what was written -- proving the offset targets the temporary
  // fixture blob, not some other/live storage.
  *(float *)out_ptr = 0.42f;

  const dt_introspection_field_t *out_field2 = NULL;
  void *out_ptr2 = NULL;
  assert_true(dt_remote_path_resolve(&curve_node_x_path, intro->field, fixture,
                                      &out_field2, &out_ptr2, &err));
  assert_null(err);
  assert_ptr_equal(out_ptr2, out_ptr);
  assert_float_equal(*(float *)out_ptr2, 0.42f, 1e-6);

  g_free(fixture);
}

static void test_path_resolve_curve_num_nodes(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);

  const dt_introspection_field_t *out_field = NULL;
  void *out_ptr = NULL;
  dt_remote_error_t *err = NULL;

  assert_true(dt_remote_path_resolve(&curve_num_nodes_path, intro->field, fixture,
                                      &out_field, &out_ptr, &err));
  assert_null(err);
  assert_non_null(out_field);
  assert_non_null(out_ptr);
  assert_int_equal(out_field->header.type, DT_INTROSPECTION_TYPE_INT);

  ptrdiff_t offset = (char *)out_ptr - (char *)fixture;
  assert_true(offset >= 0);
  assert_true((size_t)offset + out_field->header.size <= intro->size);

  *(int *)out_ptr = 5;
  assert_int_equal(*(int *)out_ptr, 5);

  g_free(fixture);
}

static void test_path_resolve_rejects_wrong_type_segment(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  const dt_introspection_field_t *out_field = NULL;
  void *out_ptr = NULL;
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_path_resolve(&wrong_type_path, intro->field, fixture,
                                       &out_field, &out_ptr, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  g_free(fixture);
}

static void test_path_resolve_rejects_out_of_bounds_index(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_path_resolve(&out_of_bounds_path, intro->field, fixture,
                                       NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  g_free(fixture);
}

static void test_path_resolve_rejects_missing_child(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_path_resolve(&missing_child_path, intro->field, fixture,
                                       NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  g_free(fixture);
}

static void test_path_resolve_rejects_index_into_non_array(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_path_resolve(&index_into_non_array_path, intro->field, fixture,
                                       NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  g_free(fixture);
}

static void test_path_resolve_rejects_null_arguments(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  dt_remote_error_t *err = NULL;

  assert_false(dt_remote_path_resolve(NULL, intro->field, fixture, NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
  err = NULL;

  assert_false(dt_remote_path_resolve(&curve_num_nodes_path, NULL, fixture, NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
  err = NULL;

  assert_false(dt_remote_path_resolve(&curve_num_nodes_path, intro->field, NULL, NULL, NULL, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  g_free(fixture);
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
    cmocka_unit_test(test_curve_validate_accepts_well_formed_points),
    cmocka_unit_test(test_curve_validate_rejects_too_few_points),
    cmocka_unit_test(test_curve_validate_rejects_too_many_points),
    cmocka_unit_test(test_curve_validate_rejects_nan),
    cmocka_unit_test(test_curve_validate_rejects_positive_infinity),
    cmocka_unit_test(test_curve_validate_rejects_x_outside_domain),
    cmocka_unit_test(test_curve_validate_rejects_y_outside_domain),
    cmocka_unit_test(test_curve_validate_never_mutates_points_on_rejection),
    cmocka_unit_test(test_curve_validate_rejects_descending_x),
    cmocka_unit_test(test_curve_validate_rejects_duplicate_x),
    cmocka_unit_test(test_curve_validate_rejects_spacing_exactly_at_threshold_greater_than),
    cmocka_unit_test(test_curve_validate_accepts_spacing_just_above_threshold),
    cmocka_unit_test(test_curve_validate_accepts_spacing_exactly_at_threshold_at_least),
    cmocka_unit_test(test_curve_validate_rejects_spacing_below_threshold_at_least),
    cmocka_unit_test(test_curve_validate_spacing_none_skips_adjacent_check),
    cmocka_unit_test(test_curve_validate_rejects_wrap_spacing_below_threshold),
    cmocka_unit_test(test_curve_validate_accepts_wrap_spacing_above_threshold),
    cmocka_unit_test(test_curve_validate_wrap_spacing_none_skips_wrap_check),
    cmocka_unit_test(test_curve_validate_optional_boundary_accepts_interior_endpoints),
    cmocka_unit_test(test_curve_validate_required_boundary_accepts_domain_edges),
    cmocka_unit_test(test_curve_validate_required_boundary_rejects_first_off_minimum),
    cmocka_unit_test(test_curve_validate_required_boundary_rejects_last_off_maximum),
    cmocka_unit_test(test_curve_validate_fixed_identity_accepts_identity_endpoints),
    cmocka_unit_test(test_curve_validate_fixed_identity_rejects_first_y_off_minimum),
    cmocka_unit_test(test_curve_validate_fixed_identity_rejects_last_y_off_maximum),
    cmocka_unit_test(test_curve_validate_accepts_allowed_interpolation),
    cmocka_unit_test(test_curve_validate_rejects_disallowed_interpolation),
    cmocka_unit_test(test_curve_validate_rejects_negative_interpolation_without_mutating_points),
    cmocka_unit_test(test_curve_validate_rejects_oversized_interpolation_without_mutating_points),
    cmocka_unit_test(test_curve_validate_omitted_interpolation_skips_allowlist_check),
    cmocka_unit_test(test_rgbcurve_adapter_linked_mode_exposes_only_curve_master),
    cmocka_unit_test(test_rgbcurve_adapter_manual_mode_exposes_rgb_not_master),
    cmocka_unit_test(test_rgbcurve_adapter_native_channel_zero_round_trips_under_correct_id),
    cmocka_unit_test(test_rgbcurve_adapter_all_interpolation_names_map_correctly),
    cmocka_unit_test(test_rgbcurve_adapter_mode_transition_multi_curve_patch_is_atomic),
    cmocka_unit_test(test_rgbcurve_adapter_interior_endpoints_round_trip_without_normalization),
    cmocka_unit_test(test_rgbcurve_adapter_entering_manual_copies_master_before_explicit_replacement),
    cmocka_unit_test(test_rgbcurve_adapter_middle_grey_transforms_untouched_before_explicit_replacement),
    cmocka_unit_test(test_rgbcurve_adapter_invalid_green_blue_write_in_linked_mode_is_unsupported),
    cmocka_unit_test(test_rgbcurve_adapter_unused_native_capacity_is_zero_and_not_returned),
    cmocka_unit_test(test_tonecurve_adapter_ab_write_in_linked_mode_is_unsupported),
    cmocka_unit_test(test_tonecurve_adapter_mode_flip_and_ab_write_in_one_patch),
    cmocka_unit_test(test_path_resolve_curve_node_x),
    cmocka_unit_test(test_path_resolve_curve_num_nodes),
    cmocka_unit_test(test_path_resolve_rejects_wrong_type_segment),
    cmocka_unit_test(test_path_resolve_rejects_out_of_bounds_index),
    cmocka_unit_test(test_path_resolve_rejects_missing_child),
    cmocka_unit_test(test_path_resolve_rejects_index_into_non_array),
    cmocka_unit_test(test_path_resolve_rejects_null_arguments),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
