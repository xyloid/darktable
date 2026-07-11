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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_curve.h"
#include "control/remote_edit.h"
#include "control/remote_parameters.h"

#include "develop/imageop.h" // dt_iop_get_module_so()/dt_iop_module_so_t

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
