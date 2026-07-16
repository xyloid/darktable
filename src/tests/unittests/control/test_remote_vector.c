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
 * cmocka unit tests for the vector-class engine and registry core
 * (src/control/remote_vector.c/.h, src/control/remote_vector_registry.c),
 * mirroring test_remote_curve.c/test_remote_curve_registry.c's structure and
 * fixture idioms for the curve twin. The production adapter table
 * (remote_vector_registry.c's s_adapters[]) is empty in this task -- every
 * behavior below is proven with a test-local, hand-rolled adapter/descriptor
 * against the real "borders" module .so (a simple `color[3]` float field),
 * installed through dt_remote_vector_registry_set_lookup_override() (the
 * same test seam dt_remote_curve_registry_set_lookup_override() provides
 * for the curve registry).
 *
 * Coverage:
 *  - dt_remote_vector_schema_free()/dt_remote_vector_value_free() on fully
 *    populated, partially populated, and NULL instances.
 *  - dt_remote_vector_validate(): pure, count mismatch, per-component
 *    domain (including the double-precision boundary that is the whole
 *    reason values travel as doubles), LEVELS ordering and minimum-gap
 *    rules.
 *  - dt_remote_vector_registry_validate(): native path shape, native
 *    capacity vs. component_count, duplicate IDs within one adapter,
 *    cross-class collisions with the curve registry, COLOR/LEVELS
 *    structural requirements.
 *  - dt_remote_vector_list_schema(): descriptor -> owned schema conversion
 *    in registry order; no-adapter silent degrade (NULL, not empty array).
 *  - dt_remote_vector_read_values(): float->double widening, active/
 *    writable_now stamped from predicates evaluated against the live
 *    params blob.
 *  - dt_remote_vector_apply_patch(): unknown ID, duplicate ID, count
 *    mismatch, domain rejection (with the live params left untouched),
 *    writable_when evaluated against the post-scalar-write projected
 *    block, narrowing writes that leave the preserved tail and every
 *    other params byte untouched, validate_completed rollback, and mixed
 *    patches where non-vector semantic entries are silently skipped.
 *
 * Please see README.md for more detailed documentation.
 */
#include <float.h>
#include <math.h>
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
#include "control/remote_vector.h"

#include "develop/develop.h"
#include "develop/imageop.h" // dt_iop_get_module_so()/dt_iop_module_so_t/dt_iop_module_t

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_schema_free                                            */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *make_condition(const char *field, const char *enum_name)
{
  dt_remote_parameter_condition_t *c = g_new0(dt_remote_parameter_condition_t, 1);
  c->field = g_strdup(field);
  c->op = DT_REMOTE_PREDICATE_EQ;
  c->enum_name = g_strdup(enum_name);
  return c;
}

static GArray *make_component_schemas(void)
{
  GArray *components = g_array_new(FALSE, FALSE, sizeof(dt_remote_vector_component_schema_t));
  dt_remote_vector_component_schema_t red = { .name = g_strdup("red"), .minimum = 0.0, .maximum = 1.0 };
  dt_remote_vector_component_schema_t green = { .name = g_strdup("green"), .minimum = 0.0, .maximum = 1.0 };
  g_array_append_val(components, red);
  g_array_append_val(components, green);
  return components;
}

static void test_vector_schema_free_fully_populated(void **state)
{
  (void)state;
  dt_remote_vector_schema_t *schema = g_new0(dt_remote_vector_schema_t, 1);
  schema->name = g_strdup("vector.color");
  schema->display_name = g_strdup("Color");
  schema->description = g_strdup("the border color");
  schema->subtype = DT_REMOTE_VECTOR_COLOR;
  schema->color_space = g_strdup("display_rgb");
  schema->components = make_component_schemas();
  schema->strictly_increasing = FALSE;
  schema->minimum_gap = 0.0;
  schema->writability = DT_REMOTE_WRITABLE_CONDITIONAL;
  schema->active_when = make_condition("mode", "MODE_ON");
  schema->writable_when = make_condition("mode", "MODE_ON");

  dt_remote_vector_schema_free(schema);
  // no crash/double-free is the assertion; cmocka has nothing else to check
}

static void test_vector_schema_free_partially_populated(void **state)
{
  (void)state;
  // description, color_space, components, and the two conditions are all
  // documented nullable; leave them unset to exercise the NULL branches.
  dt_remote_vector_schema_t *schema = g_new0(dt_remote_vector_schema_t, 1);
  schema->name = g_strdup("vector.color");
  schema->display_name = g_strdup("Color");
  schema->writability = DT_REMOTE_WRITABLE_NOW;

  dt_remote_vector_schema_free(schema);
}

static void test_vector_schema_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_vector_schema_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_value_free                                             */
/* ---------------------------------------------------------------------- */

static GArray *make_values(const double *values, guint n)
{
  GArray *array = g_array_sized_new(FALSE, FALSE, sizeof(double), n);
  g_array_append_vals(array, values, n);
  return array;
}

static void test_vector_value_free_fully_populated(void **state)
{
  (void)state;
  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_vector_value_t *value = g_new0(dt_remote_vector_value_t, 1);
  value->name = g_strdup("vector.color");
  value->values = make_values(values, G_N_ELEMENTS(values));
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;

  dt_remote_vector_value_free(value);
}

static void test_vector_value_free_partially_populated(void **state)
{
  (void)state;
  dt_remote_vector_value_t *value = g_new0(dt_remote_vector_value_t, 1);
  value->name = g_strdup("vector.color");

  dt_remote_vector_value_free(value);
}

static void test_vector_value_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_vector_value_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_validate                                               */
/* ---------------------------------------------------------------------- */

/*
 * dt_remote_vector_validate() is pure (no introspection, no registry, no
 * JSON parsing of the input), so these tests build dt_remote_vector_descriptor_t
 * instances by hand -- no live module or dt_init() dependency.
 */

static const dt_remote_vector_component_t s_plain_components[] = {
  { .name = "red", .minimum = 0.0, .maximum = 1.0 },
  { .name = "green", .minimum = 0.0, .maximum = 2.0 },
  { .name = "blue", .minimum = -1.0, .maximum = 1.0 },
};

static dt_remote_vector_descriptor_t make_plain_descriptor(void)
{
  dt_remote_vector_descriptor_t desc = { 0 };
  desc.name = "vector.plain";
  desc.component_count = G_N_ELEMENTS(s_plain_components);
  desc.components = s_plain_components;
  desc.native_capacity = G_N_ELEMENTS(s_plain_components);
  desc.subtype = DT_REMOTE_VECTOR_PLAIN;
  return desc;
}

static void assert_vector_error_details(dt_remote_error_t *err, const char *expected_parameter,
                                        int expected_component_index, const char *expected_constraint)
{
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(err->details_json);
  assert_non_null(strstr(err->details_json, expected_parameter));
  assert_non_null(strstr(err->details_json, expected_constraint));
  if(expected_component_index >= 0)
  {
    char needle[32];
    g_snprintf(needle, sizeof(needle), "\"component_index\":%d", expected_component_index);
    assert_non_null(strstr(err->details_json, needle));
  }
  else
  {
    assert_null(strstr(err->details_json, "component_index"));
  }
}

static void test_vector_validate_accepts_well_formed_values(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { 0.5, 1.5, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

static void test_vector_validate_rejects_count_mismatch(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { 0.5, 1.5 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.plain", -1, "count_mismatch");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_rejects_component_below_minimum(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { -0.1, 1.5, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.plain", 0, "domain");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

// The reason components travel as doubles end-to-end: a value that would
// round-trip through float32 identically to the maximum must still be
// rejected when it is genuinely larger in double precision.
static void test_vector_validate_rejects_component_above_maximum_at_double_boundary(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { 0.5, 2.00000001, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.plain", 1, "domain");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_accepts_component_exactly_at_maximum(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { 1.0, 2.0, 1.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

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

static void test_vector_validate_never_mutates_values_on_rejection(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_plain_descriptor();
  static const double values[] = { 0.5, 3.0, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));
  GArray *snapshot = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_non_null(err);
  assert_memory_equal(array->data, snapshot->data, array->len * sizeof(double));

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
  g_array_free(snapshot, TRUE);
}

// VEC3-013: a component whose declared bounds are wider than the native
// float range (never possible through the registry after that fix -- see
// test_registry_validate_rejects_invalid_descriptor_metadata's huge-bound
// cases below -- but dt_remote_vector_validate() is documented as pure and
// directly callable) must still reject a candidate that would narrow to
// +-inf, even though it falls inside those (unrealistically wide)
// double-domain bounds.
static const dt_remote_vector_component_t s_wide_bound_components[] = {
  { .name = "red", .minimum = -1e39, .maximum = 1e39 },
  { .name = "green", .minimum = 0.0, .maximum = 1.0 },
  { .name = "blue", .minimum = -1.0, .maximum = 1.0 },
};

static dt_remote_vector_descriptor_t make_wide_bound_descriptor(void)
{
  dt_remote_vector_descriptor_t desc = { 0 };
  desc.name = "vector.wide";
  desc.component_count = G_N_ELEMENTS(s_wide_bound_components);
  desc.components = s_wide_bound_components;
  desc.native_capacity = G_N_ELEMENTS(s_wide_bound_components);
  desc.subtype = DT_REMOTE_VECTOR_PLAIN;
  return desc;
}

static void test_vector_validate_rejects_candidate_exceeding_native_float_range(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_wide_bound_descriptor();
  static const double values[] = { 1e39, 0.5, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));
  GArray *snapshot = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.wide", 0, "native_range");
  assert_memory_equal(array->data, snapshot->data, array->len * sizeof(double));

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
  g_array_free(snapshot, TRUE);
}

static void test_vector_validate_rejects_negative_candidate_exceeding_native_float_range(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_wide_bound_descriptor();
  static const double values[] = { -1e39, 0.5, 0.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.wide", 0, "native_range");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_accepts_candidate_exactly_at_native_float_range_boundary(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_wide_bound_descriptor();
  static const double values[] = { (double)FLT_MAX, 0.5, -1.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

static void test_vector_validate_accepts_candidate_exactly_at_negative_native_float_range_boundary(
  void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_wide_bound_descriptor();
  static const double values[] = { -(double)FLT_MAX, 0.5, -1.0 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

static const dt_remote_vector_component_t s_levels_components[] = {
  { .name = "black", .minimum = 0.0, .maximum = 1.0 },
  { .name = "mid", .minimum = 0.0, .maximum = 1.0 },
  { .name = "white", .minimum = 0.0, .maximum = 1.0 },
};

static dt_remote_vector_descriptor_t make_levels_descriptor(double minimum_gap)
{
  dt_remote_vector_descriptor_t desc = { 0 };
  desc.name = "vector.levels";
  desc.component_count = G_N_ELEMENTS(s_levels_components);
  desc.components = s_levels_components;
  desc.native_capacity = G_N_ELEMENTS(s_levels_components);
  desc.subtype = DT_REMOTE_VECTOR_LEVELS;
  desc.strictly_increasing = TRUE;
  desc.minimum_gap = minimum_gap;
  return desc;
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

static void test_vector_validate_levels_accepts_strictly_increasing(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor(0.01);
  static const double values[] = { 0.1, 0.5, 0.9 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

static void test_vector_validate_levels_rejects_unordered(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor(0.01);
  static const double values[] = { 0.5, 0.3, 0.9 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.levels", 1, "unordered");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_levels_rejects_equal_adjacent_values(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor(0.01);
  static const double values[] = { 0.5, 0.5, 0.9 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.levels", 1, "unordered");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_levels_rejects_gap_below_flt_epsilon(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor((double)FLT_EPSILON);
  static const double values[3] = { 0.1, 0.1 + (double)FLT_EPSILON * 0.5, 0.9 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_validate(&desc, array, &err));
  assert_vector_error_details(err, "vector.levels", 1, "gap");

  dt_remote_error_free(err);
  g_array_free(array, TRUE);
}

static void test_vector_validate_levels_accepts_gap_exactly_at_flt_epsilon(void **state)
{
  (void)state;
  dt_remote_vector_descriptor_t desc = make_levels_descriptor((double)FLT_EPSILON);
  static const double values[3] = { 0.1, 0.1 + (double)FLT_EPSILON, 0.9 };
  GArray *array = make_values(values, G_N_ELEMENTS(values));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_validate(&desc, array, &err));
  assert_null(err);

  g_array_free(array, TRUE);
}

/* ---------------------------------------------------------------------- */
/* harness: real "borders" module .so via dt_init()                        */
/* ---------------------------------------------------------------------- */

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_harness_confdir = NULL;
static GPtrArray *s_registry_test_allocations = NULL;
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

static int harness_group_setup(void **state)
{
  (void)state;
  s_registry_test_allocations = g_ptr_array_new_with_free_func(g_free);
  GError *gerror = NULL;
  s_harness_confdir = g_dir_make_tmp("test_remote_vector-XXXXXX", &gerror);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_vector: failed to create scratch config dir: %s\n", gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_vector",
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
  g_clear_pointer(&s_registry_test_allocations, g_ptr_array_unref);
  if(s_harness_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_harness_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_vector: failed to remove scratch config dir %s\n", s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

static int lookup_override_test_setup(void **state)
{
  (void)state;
  dt_remote_vector_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  return 0;
}

static int lookup_override_test_teardown(void **state)
{
  (void)state;
  dt_remote_vector_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  s_private_vector_intro = NULL;
  return 0;
}

/* ---------------------------------------------------------------------- */
/* borders fixture: descriptor/adapter data, module loading                */
/* ---------------------------------------------------------------------- */

typedef struct borders_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} borders_fixture_t;

// Generic real-module loader: despite the type name (kept for the many
// existing borders-specific callers below), nothing here is borders-
// specific -- same "adapter_fixture_new(op)" generalization
// test_remote_curve.c uses for its own rgbcurve_fixture_t across
// rgbcurve/tonecurve/colorzones/basecurve. The colorbalance adapter
// section further below reuses this loader directly rather than adding a
// parallel fixture type.
static borders_fixture_t *real_vector_module_fixture_new(const char *op)
{
  borders_fixture_t *fixture = g_new0(borders_fixture_t, 1);
  dt_dev_init(&fixture->dev, TRUE);
  fixture->dev.gui_attached = FALSE;
  fixture->module = g_malloc0(sizeof(dt_iop_module_t));
  dt_iop_module_so_t *so = dt_iop_get_module_so(op);
  assert_non_null(so);
  assert_false(dt_iop_load_module(fixture->module, so, &fixture->dev));
  memcpy(fixture->module->params, fixture->module->default_params, fixture->module->params_size);
  fixture->dev.iop = g_list_append(fixture->dev.iop, fixture->module);
  return fixture;
}

static borders_fixture_t *borders_fixture_new(void)
{
  return real_vector_module_fixture_new("borders");
}

static void borders_fixture_free(borders_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

// "color" (border color, float[3]) -- the generic fixture field.
static const dt_remote_path_segment_t s_color_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "color" },
};
static const dt_remote_introspection_path_t s_color_path = {
  .segments = s_color_segments, .length = G_N_ELEMENTS(s_color_segments)
};

// "frame_color" (frame line color, float[3]) -- a second, independent
// float[3] field used where a test needs a distinct native leaf from
// "color" (e.g. so two descriptors in the same adapter do not alias).
static const dt_remote_path_segment_t s_frame_color_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "frame_color" },
};
static const dt_remote_introspection_path_t s_frame_color_path = {
  .segments = s_frame_color_segments, .length = G_N_ELEMENTS(s_frame_color_segments)
};

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

static const dt_remote_vector_component_t s_rgb_components[] = {
  { .name = "red", .minimum = 0.0, .maximum = 1.0 },
  { .name = "green", .minimum = 0.0, .maximum = 1.0 },
  { .name = "blue", .minimum = 0.0, .maximum = 1.0 },
};

static const dt_remote_vector_component_t s_rg_components[] = {
  { .name = "red", .minimum = 0.0, .maximum = 1.0 },
  { .name = "green", .minimum = 0.0, .maximum = 1.0 },
};

static const dt_remote_vector_component_t s_rgba_components[] = {
  { "red", 0.0, 1.0 }, { "green", 0.0, 1.0 },
  { "blue", 0.0, 1.0 }, { "alpha", 0.0, 1.0 },
};

static const dt_remote_vector_component_t s_levels_schema_components[] = {
  { "black", 0.0, 0.8 }, { "midpoint", 0.1, 0.9 }, { "white", 0.2, 1.0 },
};

// Base fixture descriptor: three components, full native capacity, no
// predicates, PLAIN subtype. Individual tests copy and adjust as needed.
static dt_remote_vector_descriptor_t make_color_descriptor(void)
{
  dt_remote_vector_descriptor_t desc = { 0 };
  desc.name = "vector.color";
  desc.display_name = "Color";
  desc.native = s_color_path;
  desc.component_count = G_N_ELEMENTS(s_rgb_components);
  desc.components = s_rgb_components;
  desc.native_capacity = 3;
  desc.subtype = DT_REMOTE_VECTOR_PLAIN;
  return desc;
}

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

static gboolean s_validate_completed_result = TRUE;
static int s_validate_completed_calls = 0;

static const float s_expected_completed_color[] = { 0.11f, 0.22f, 0.33f };
static const float s_expected_completed_frame[] = { 0.44f, 0.55f, 0.66f };
static gboolean s_validate_completed_force_reject = FALSE;

static gboolean test_validate_completed(const struct dt_remote_vector_context_t *ctx,
                                        const void *new_params,
                                        dt_remote_error_t **error)
{
  (void)ctx;
  (void)new_params;
  s_validate_completed_calls++;
  if(!s_validate_completed_result)
  {
    if(error)
    {
      *error = g_new0(dt_remote_error_t, 1);
      (*error)->code = DT_REMOTE_ERR_INVALID_VALUE;
      (*error)->message = g_strdup("test_validate_completed rejected");
    }
    return FALSE;
  }
  return TRUE;
}

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

static const dt_remote_vector_module_adapter_t *s_lookup_override_adapter = NULL;

static const dt_remote_vector_module_adapter_t *always_null_vector_lookup(
  const char *operation, guint params_version)
{
  (void)operation;
  (void)params_version;
  return NULL;
}

static const dt_remote_vector_module_adapter_t *private_lookup_override(const char *operation,
                                                                         guint params_version)
{
  if(!s_lookup_override_adapter || g_strcmp0(operation, s_lookup_override_adapter->operation)
     || params_version < s_lookup_override_adapter->minimum_params_version
     || params_version > s_lookup_override_adapter->maximum_params_version)
    return NULL;
  return s_lookup_override_adapter;
}

static void install_vector_adapter(const dt_remote_vector_module_adapter_t *adapter)
{
  s_lookup_override_adapter = adapter;
  dt_remote_vector_registry_set_lookup_override(private_lookup_override);
}

// A synthetic curve adapter installed only for the cross-class collision
// test below; its descriptors are never resolved against real
// introspection (dt_remote_vector_registry_validate() only reads their
// `.name`), so a minimal, unvalidated curve descriptor is sufficient.
static const dt_remote_curve_descriptor_t s_colliding_curve_descriptor = {
  .name = "vector.color", // deliberately identical to make_color_descriptor()'s name
};
static const dt_remote_curve_module_adapter_t s_colliding_curve_adapter = {
  .operation = "borders",
  .minimum_params_version = 4,
  .maximum_params_version = 4,
  .curves = &s_colliding_curve_descriptor,
  .curve_count = 1,
};
static const dt_remote_curve_module_adapter_t *colliding_curve_lookup_override(const char *operation,
                                                                                guint params_version)
{
  if(g_strcmp0(operation, "borders") || params_version != 4) return NULL;
  return &s_colliding_curve_adapter;
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_registry_validate                                      */
/* ---------------------------------------------------------------------- */

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

  // VEC3-013: a finite double bound outside the native float range must be
  // rejected -- accepting it would let a candidate at the same bound pass
  // dt_remote_vector_validate()'s domain check and then narrow to +-inf in
  // the write path's `(float)value` conversion.
  static const dt_remote_vector_component_t huge_maximum[] = {
    { "red", 0.0, 1e39 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_huge_maximum = new_test_adapter(&descriptor);
  descriptor->components = huge_maximum;
  assert_vector_registry_rejects(bad_huge_maximum, so);

  static const dt_remote_vector_component_t huge_minimum[] = {
    { "red", -1e39, 1.0 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  dt_remote_vector_module_adapter_t *bad_huge_minimum = new_test_adapter(&descriptor);
  descriptor->components = huge_minimum;
  assert_vector_registry_rejects(bad_huge_minimum, so);
}

static void test_registry_validate_accepts_component_bounds_at_native_float_range_boundary(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static const dt_remote_vector_component_t boundary_bounds[] = {
    { "red", -(double)FLT_MAX, (double)FLT_MAX }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.components = boundary_bounds;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_null(err);
}

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

static void test_registry_validate_rejects_color_with_nonzero_minimum_gap(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_remote_vector_descriptor_t *descriptor = NULL;

  dt_remote_vector_module_adapter_t *color = new_test_adapter(&descriptor);
  descriptor->subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor->color_space = "display_rgb";
  descriptor->minimum_gap = FLT_EPSILON;
  assert_vector_registry_rejects(color, so);
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

static void assert_registry_rejects_private_color_array(gboolean has_element_descriptor,
                                                        dt_introspection_type_t element_type,
                                                        size_t element_size,
                                                        size_t array_count)
{
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_introspection_t broken_intro = *so->get_introspection();
  dt_introspection_field_t broken_root = *broken_intro.field;
  dt_introspection_field_t broken_color = { 0 };
  dt_introspection_field_t broken_element = { 0 };
  gboolean found_color = FALSE;
  dt_introspection_field_t **fields =
    g_new(dt_introspection_field_t *, broken_root.Struct.entries + 1);
  for(guint i = 0; i <= broken_root.Struct.entries; i++)
  {
    fields[i] = broken_root.Struct.fields[i];
    if(fields[i] && !g_strcmp0(fields[i]->header.field_name, "color"))
    {
      broken_color = *fields[i];
      assert_non_null(broken_color.Array.field);
      broken_element = *broken_color.Array.field;
      broken_element.header.type = element_type;
      broken_element.header.size = element_size;
      broken_color.Array.field = has_element_descriptor ? &broken_element : NULL;
      broken_color.Array.count = array_count;
      fields[i] = &broken_color;
      found_color = TRUE;
    }
  }
  assert_true(found_color);
  broken_root.Struct.fields = fields;
  broken_intro.field = &broken_root;
  dt_iop_module_so_t private_so;
  init_private_vector_so(&private_so, "borders", &broken_intro);
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *adapter = new_test_adapter(&descriptor);
  dt_remote_error_t *error = NULL;
  const gboolean valid = dt_remote_vector_registry_validate(adapter, &private_so, &error);
  s_private_vector_intro = NULL;
  g_free(fields);

  assert_false(valid);
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

static void test_registry_validate_rejects_missing_float_element_descriptor(void **state)
{
  (void)state;
  assert_registry_rejects_private_color_array(FALSE, DT_INTROSPECTION_TYPE_FLOAT,
                                              sizeof(float), 3);
}

static void test_registry_validate_rejects_nonfloat_element_descriptor(void **state)
{
  (void)state;
  assert_registry_rejects_private_color_array(TRUE, DT_INTROSPECTION_TYPE_CHAR,
                                              sizeof(float), 3);
}

static void test_registry_validate_rejects_wrong_float_element_size(void **state)
{
  (void)state;
  assert_registry_rejects_private_color_array(TRUE, DT_INTROSPECTION_TYPE_FLOAT,
                                              sizeof(float) + 1, 3);
}

static void test_registry_validate_rejects_oversized_native_count(void **state)
{
  (void)state;
  if(sizeof(size_t) <= sizeof(guint)) return;

  const size_t oversized_count =
    (size_t)G_MAXUINT + 1 + G_N_ELEMENTS(s_rgb_components);
  assert_registry_rejects_private_color_array(TRUE, DT_INTROSPECTION_TYPE_FLOAT,
                                              sizeof(float), oversized_count);
}

// VEC3-012: element count/type/size can all individually look correct
// while the aggregate array leaf's own declared byte size is too small to
// actually hold that many elements -- dt_introspection_access_array()
// addresses elements as `start + element * Array.field->header.size`
// bounded only by Array.count, so an undersized aggregate leaf validates
// today and then permits out-of-bounds addressing. Mirrors
// assert_registry_rejects_private_color_array() above, but corrupts the
// ARRAY field's own header.size rather than its element descriptor.
static void assert_registry_rejects_private_color_array_with_aggregate_size(size_t aggregate_size_override)
{
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_introspection_t broken_intro = *so->get_introspection();
  dt_introspection_field_t broken_root = *broken_intro.field;
  dt_introspection_field_t broken_color = { 0 };
  gboolean found_color = FALSE;
  dt_introspection_field_t **fields =
    g_new(dt_introspection_field_t *, broken_root.Struct.entries + 1);
  for(guint i = 0; i <= broken_root.Struct.entries; i++)
  {
    fields[i] = broken_root.Struct.fields[i];
    if(fields[i] && !g_strcmp0(fields[i]->header.field_name, "color"))
    {
      broken_color = *fields[i];
      assert_non_null(broken_color.Array.field);
      // Element descriptor/type/size and Array.count are left exactly as
      // the real "color" field declares them (3 sizeof(float) elements);
      // only the aggregate leaf's own byte size is corrupted, below what
      // 3 floats require.
      broken_color.header.size = aggregate_size_override;
      fields[i] = &broken_color;
      found_color = TRUE;
    }
  }
  assert_true(found_color);
  broken_root.Struct.fields = fields;
  broken_intro.field = &broken_root;
  dt_iop_module_so_t private_so;
  init_private_vector_so(&private_so, "borders", &broken_intro);
  dt_remote_vector_descriptor_t *descriptor = NULL;
  dt_remote_vector_module_adapter_t *adapter = new_test_adapter(&descriptor);
  dt_remote_error_t *error = NULL;
  const gboolean valid = dt_remote_vector_registry_validate(adapter, &private_so, &error);
  s_private_vector_intro = NULL;
  g_free(fields);

  assert_false(valid);
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

static void test_registry_validate_rejects_undersized_aggregate_array_byte_size(void **state)
{
  (void)state;
  assert_registry_rejects_private_color_array_with_aggregate_size(sizeof(float) * 2);
}

static void test_registry_validate_passes_for_well_formed_descriptor(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_null(err);
}

static void test_registry_validate_rejects_native_capacity_mismatch(void **state)
{
  (void)state;
  // component_count (2) fits within "color"'s real length (3), but the
  // descriptor's declared native_capacity (5) does not match the resolved
  // array length -- must fail even though nothing would overflow.
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.component_count = 2;
  descriptor.components = s_rg_components;
  descriptor.native_capacity = 5;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
}

static void test_registry_validate_rejects_duplicate_names_within_adapter(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptors[2];
  descriptors[0] = make_color_descriptor();
  descriptors[1] = make_color_descriptor();
  descriptors[1].native = s_frame_color_path; // distinct native leaf, same name
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = descriptors, .vector_count = 2,
  };

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
}

static void test_registry_validate_rejects_name_colliding_with_curve_registry(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  dt_remote_curve_registry_set_lookup_override(colliding_curve_lookup_override);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor(); // name "vector.color", matches the synthetic curve descriptor
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);

  dt_remote_curve_registry_set_lookup_override(NULL);
}

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

static void test_registry_validate_rejects_color_without_color_space(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor.color_space = NULL;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
}

static void test_registry_validate_accepts_color_with_color_space(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.subtype = DT_REMOTE_VECTOR_COLOR;
  descriptor.color_space = "display_rgb";
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_null(err);
}

static void test_registry_validate_rejects_levels_without_strictly_increasing(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.subtype = DT_REMOTE_VECTOR_LEVELS;
  descriptor.strictly_increasing = FALSE;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_vector_registry_validate(&adapter, so, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_list_schema                                            */
/* ---------------------------------------------------------------------- */

static void test_list_schema_no_adapter_yields_null_and_succeeds(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  GPtrArray *sentinel = g_ptr_array_new();
  GPtrArray *fields = sentinel;
  dt_remote_error_t *error = NULL;
  dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);
  assert_true(dt_remote_vector_list_schema(so, &fields, &error));
  assert_null(error);
  assert_null(fields);
  g_ptr_array_unref(sentinel);
}

static void test_list_schema_invalid_native_layout_clears_output(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.component_count = G_N_ELEMENTS(s_rgba_components);
  descriptor.components = s_rgba_components;
  descriptor.native_capacity = 3;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  GPtrArray *sentinel = g_ptr_array_new();
  GPtrArray *fields = sentinel;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_vector_list_schema(so, &fields, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(fields);

  dt_remote_error_free(error);
  g_ptr_array_unref(sentinel);
}

static const dt_remote_parameter_predicate_t active_predicate = {
  .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
  .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT",
};
static const dt_remote_parameter_predicate_t writable_predicate = {
  .field = "basis", .op = DT_REMOTE_PREDICATE_NE,
  .enum_name = "DT_IOP_BORDERS_BASIS_WIDTH",
};

static void assert_schema_condition_matches(
  const dt_remote_parameter_condition_t *condition,
  const dt_remote_parameter_predicate_t *predicate)
{
  assert_non_null(condition);
  assert_ptr_not_equal(condition->field, predicate->field);
  assert_string_equal(condition->field, predicate->field);
  assert_int_equal(condition->op, predicate->op);
  assert_ptr_not_equal(condition->enum_name, predicate->enum_name);
  assert_string_equal(condition->enum_name, predicate->enum_name);
}

static void assert_schema_components_match(
  const dt_remote_vector_schema_t *schema,
  const dt_remote_vector_descriptor_t *descriptor)
{
  assert_non_null(schema->components);
  assert_int_equal(schema->components->len, descriptor->component_count);
  for(guint i = 0; i < descriptor->component_count; i++)
  {
    const dt_remote_vector_component_schema_t component =
      g_array_index(schema->components, dt_remote_vector_component_schema_t, i);
    assert_ptr_not_equal(component.name, descriptor->components[i].name);
    assert_string_equal(component.name, descriptor->components[i].name);
    assert_float_equal(component.minimum, descriptor->components[i].minimum, 0.0);
    assert_float_equal(component.maximum, descriptor->components[i].maximum, 0.0);
  }
}

static void test_list_schema_converts_descriptors_in_registry_order(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptors[2];
  descriptors[0] = make_color_descriptor();
  descriptors[0].name = "vector.first";
  descriptors[0].description = "first description";
  descriptors[0].subtype = DT_REMOTE_VECTOR_COLOR;
  descriptors[0].color_space = "display_rgb";
  descriptors[0].active_when = &active_predicate;
  descriptors[0].writable_when = &writable_predicate;
  descriptors[1] = make_color_descriptor();
  descriptors[1].name = "vector.second";
  descriptors[1].display_name = "Levels";
  descriptors[1].description = "second description";
  descriptors[1].native = s_frame_color_path;
  descriptors[1].components = s_levels_schema_components;
  descriptors[1].subtype = DT_REMOTE_VECTOR_LEVELS;
  descriptors[1].strictly_increasing = TRUE;
  descriptors[1].minimum_gap = FLT_EPSILON;
  descriptors[1].color_space = NULL;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = descriptors, .vector_count = 2,
  };
  install_vector_adapter(&adapter);

  GPtrArray *fields = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_list_schema(so, &fields, &err));
  assert_null(err);
  assert_non_null(fields);
  assert_int_equal(fields->len, 2);

  dt_remote_vector_schema_t *first = g_ptr_array_index(fields, 0);
  dt_remote_vector_schema_t *second = g_ptr_array_index(fields, 1);
  assert_ptr_not_equal(first->name, descriptors[0].name);
  assert_string_equal(first->name, "vector.first");
  assert_ptr_not_equal(first->display_name, descriptors[0].display_name);
  assert_string_equal(first->display_name, "Color");
  assert_ptr_not_equal(first->description, descriptors[0].description);
  assert_string_equal(first->description, "first description");
  assert_int_equal(first->subtype, DT_REMOTE_VECTOR_COLOR);
  assert_ptr_not_equal(first->color_space, descriptors[0].color_space);
  assert_string_equal(first->color_space, "display_rgb");
  assert_schema_components_match(first, &descriptors[0]);
  assert_false(first->strictly_increasing);
  assert_float_equal(first->minimum_gap, 0.0, 0.0);
  assert_int_equal(first->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
  assert_ptr_not_equal(first->active_when, descriptors[0].active_when);
  assert_ptr_not_equal(first->writable_when, descriptors[0].writable_when);
  assert_schema_condition_matches(first->active_when, &active_predicate);
  assert_schema_condition_matches(first->writable_when, &writable_predicate);

  assert_ptr_not_equal(second->name, descriptors[1].name);
  assert_string_equal(second->name, "vector.second");
  assert_ptr_not_equal(second->display_name, descriptors[1].display_name);
  assert_string_equal(second->display_name, "Levels");
  assert_ptr_not_equal(second->description, descriptors[1].description);
  assert_string_equal(second->description, "second description");
  assert_int_equal(second->subtype, DT_REMOTE_VECTOR_LEVELS);
  assert_null(second->color_space);
  assert_schema_components_match(second, &descriptors[1]);
  assert_true(second->strictly_increasing);
  assert_float_equal(second->minimum_gap, (double)FLT_EPSILON, 0.0);
  assert_int_equal(second->writability, DT_REMOTE_WRITABLE_NOW);
  assert_null(second->active_when);
  assert_null(second->writable_when);

  // The public array contract installs dt_remote_vector_schema_free() as
  // its element destructor, exercising every populated optional member.
  g_ptr_array_unref(fields);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_read_values                                            */
/* ---------------------------------------------------------------------- */

static void init_fake_module(dt_iop_module_t *module, dt_iop_module_so_t *so)
{
  memset(module, 0, sizeof(*module));
  module->so = so;
  g_strlcpy(module->op, so->op, sizeof(module->op));
}

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

static void test_read_values_widens_floats_to_doubles(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  dt_introspection_field_t *color_field = NULL;
  float *color_ptr = dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                                fixture->module->params, "color", &color_field);
  assert_non_null(color_ptr);
  color_ptr[0] = 0.25f;
  color_ptr[1] = 0.5f;
  color_ptr[2] = 0.75f;

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &err));
  assert_null(err);
  assert_non_null(values);

  dt_remote_vector_value_t *value = g_hash_table_lookup(values, "vector.color");
  assert_non_null(value);
  assert_int_equal(value->values->len, 3);
  assert_float_equal(g_array_index(value->values, double, 0), 0.25, 1e-6);
  assert_float_equal(g_array_index(value->values, double, 1), 0.5, 1e-6);
  assert_float_equal(g_array_index(value->values, double, 2), 0.75, 1e-6);
  assert_true(value->active);
  assert_true(value->writable_now);

  g_hash_table_unref(values);
  borders_fixture_free(fixture);
}

static void test_read_values_stamps_active_and_writable_now_from_predicate(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static const dt_remote_parameter_predicate_t predicate = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT"
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.active_when = &predicate;
  descriptor.writable_when = &predicate;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  dt_introspection_field_t *orient_field = NULL;
  int *orient_ptr = dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                               fixture->module->params, "aspect_orient", &orient_field);
  assert_non_null(orient_ptr);
  int portrait_value = 0;
  assert_true(dt_introspection_get_enum_value(orient_field, "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT",
                                              &portrait_value));

  // inactive/not-writable: default (auto) orientation
  *orient_ptr = 0;
  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &err));
  assert_null(err);
  dt_remote_vector_value_t *value = g_hash_table_lookup(values, "vector.color");
  assert_non_null(value);
  assert_false(value->active);
  assert_false(value->writable_now);
  g_hash_table_unref(values);

  // active/writable: portrait orientation
  *orient_ptr = portrait_value;
  values = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &err));
  assert_null(err);
  value = g_hash_table_lookup(values, "vector.color");
  assert_non_null(value);
  assert_true(value->active);
  assert_true(value->writable_now);
  g_hash_table_unref(values);

  borders_fixture_free(fixture);
}

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

static void test_read_values_no_adapter_returns_empty_table(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_iop_module_t module;
  init_fake_module(&module, so);

  void *blob = g_malloc0(so->get_introspection()->size);

  GHashTable *sentinel = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTable *values = sentinel;
  dt_remote_error_t *error = NULL;
  dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);
  assert_true(dt_remote_vector_read_values(&module, blob, &values, &error));
  assert_null(error);
  assert_non_null(values);
  assert_ptr_not_equal(values, sentinel);
  assert_int_equal(g_hash_table_size(values), 0);

  g_hash_table_unref(values);
  g_hash_table_unref(sentinel);
  g_free(blob);
}

static void test_read_values_invalid_native_layout_clears_output(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.component_count = G_N_ELEMENTS(s_rgba_components);
  descriptor.components = s_rgba_components;
  descriptor.native_capacity = 3;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  GHashTable *sentinel = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTable *values = sentinel;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_vector_read_values(fixture->module, fixture->module->params,
                                            &values, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(values);

  dt_remote_error_free(error);
  g_hash_table_unref(sentinel);
  borders_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_vector_apply_patch                                            */
/* ---------------------------------------------------------------------- */

static dt_remote_semantic_patch_t *make_vector_patch(const char *name, const double *values, guint count)
{
  dt_remote_semantic_patch_t *semantic = g_new0(dt_remote_semantic_patch_t, 1);
  semantic->class_id = DT_REMOTE_PARAMETER_VECTOR;
  semantic->value.vector.name = g_strdup(name);
  semantic->value.vector.values = g_array_sized_new(FALSE, FALSE, sizeof(double), count);
  g_array_append_vals(semantic->value.vector.values, values, count);
  return semantic;
}

static dt_remote_semantic_patch_t *make_curve_class_patch(const char *name)
{
  // Used only to prove that non-vector semantic entries are silently
  // skipped by the vector engine; never resolved against a curve adapter.
  dt_remote_semantic_patch_t *semantic = g_new0(dt_remote_semantic_patch_t, 1);
  semantic->class_id = DT_REMOTE_PARAMETER_CURVE;
  semantic->value.curve.name = g_strdup(name);
  semantic->value.curve.points = g_array_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t));
  return semantic;
}

static void vector_patch_init(dt_remote_patch_t *patch)
{
  memset(patch, 0, sizeof(*patch));
  patch->scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  patch->semantic_values = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
}

static void vector_patch_cleanup(dt_remote_patch_t *patch)
{
  g_ptr_array_unref(patch->scalar_values);
  g_ptr_array_unref(patch->semantic_values);
}

static dt_remote_patch_entry_t *make_orient_entry(const borders_fixture_t *fixture, const char *enum_name)
{
  dt_introspection_field_t *field = NULL;
  (void)dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                   fixture->module->params, "aspect_orient", &field);
  int value = 0;
  assert_true(dt_introspection_get_enum_value(field, enum_name, &value));

  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup("aspect_orient");
  entry->value.type = DT_REMOTE_VALUE_ENUM;
  entry->value.v.e.value = value;
  entry->value.v.e.name = g_strdup(enum_name);
  return entry;
}

static gboolean vector_apply_to_copy(const borders_fixture_t *fixture, const dt_remote_patch_t *patch,
                                     void *projected, dt_remote_error_t **error)
{
  memcpy(projected, fixture->module->params, fixture->module->params_size);
  dt_introspection_field_t *linear = fixture->module->so->get_introspection_linear();
  if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op(fixture->module->op), patch, projected, error))
    return FALSE;
  return dt_remote_vector_apply_patch(fixture->module, fixture->module->params, projected, patch, error);
}

static float read_color_component(const borders_fixture_t *fixture, void *params, const char *field_name,
                                  guint index)
{
  dt_introspection_field_t *array_field = NULL;
  void *array_ptr = dt_introspection_get_child(fixture->module->so->get_introspection()->field, params,
                                               field_name, &array_field);
  dt_introspection_field_t *element_field = NULL;
  float *element_ptr = dt_introspection_access_array(array_field, array_ptr, index, &element_field);
  assert_non_null(element_ptr);
  return *element_ptr;
}

static dt_remote_vector_descriptor_t s_simple_adapter_descriptor;
static dt_remote_vector_module_adapter_t s_simple_adapter;

static void install_simple_vector_adapter(void)
{
  s_simple_adapter_descriptor = make_color_descriptor();
  s_simple_adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &s_simple_adapter_descriptor, .vector_count = 1,
  };
  install_vector_adapter(&s_simple_adapter);
}

static void test_apply_patch_unknown_id_fails_with_unknown_field(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();

  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.no_such_id", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);

  dt_remote_error_free(error);
  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_no_adapter_for_op_fails_with_unknown_field(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);

  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);

  dt_remote_error_free(error);
  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_duplicate_id_fails_with_invalid_value(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();

  static const double values_a[] = { 0.1, 0.2, 0.3 };
  static const double values_b[] = { 0.4, 0.5, 0.6 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values_a, 3));
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values_b, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);

  dt_remote_error_free(error);
  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_count_mismatch_fails_with_invalid_value(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();

  static const double values[] = { 0.1, 0.2 }; // descriptor wants 3
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 2));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "count_mismatch"));

  dt_remote_error_free(error);
  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

// VEC3-013: reproduces the finding's exact scenario end to end -- a
// component bound of 1e39 (finite in double precision, but far beyond the
// native float range) paired with a candidate that exactly matches it.
// Pre-fix, both the registry's component-bound check and
// dt_remote_vector_validate()'s domain check accept this (the candidate sits
// right at the declared maximum), and the write path's `(float)value`
// conversion then silently narrows 1e39 to +inf. Post-fix, the registry
// itself rejects the 1e39 bound before any write is attempted.
static void test_apply_patch_rejects_bound_and_candidate_beyond_native_float_range(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static const dt_remote_vector_component_t huge_bound_components[] = {
    { "red", 0.0, 1e39 }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.components = huge_bound_components;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  const double values[] = { 1e39, 0.5, 0.5 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  // Fails at the registry layer (the adapter's own 1e39 bound is invalid),
  // before dt_remote_vector_validate() or the write path ever see the
  // candidate.
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

// Companion boundary-acceptance coverage: with the widest bounds a
// registry-valid descriptor may now declare (+-FLT_MAX exactly), a
// candidate exactly at FLT_MAX/-FLT_MAX is accepted and written as that
// exact float, never rejected and never narrowed to infinity.
static void test_apply_patch_accepts_candidate_exactly_at_native_float_range_boundary(void **state)
{
  (void)state;
  static const dt_remote_vector_component_t boundary_components[] = {
    { "red", -(double)FLT_MAX, (double)FLT_MAX }, { "green", 0.0, 1.0 }, { "blue", 0.0, 1.0 },
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.components = boundary_components;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };

  const double boundary_values[][3] = {
    { (double)FLT_MAX, 0.5, 0.5 },
    { -(double)FLT_MAX, 0.5, 0.5 },
  };
  const float expected_first_component[] = { FLT_MAX, -FLT_MAX };

  for(guint i = 0; i < G_N_ELEMENTS(boundary_values); i++)
  {
    borders_fixture_t *fixture = borders_fixture_new();
    install_vector_adapter(&adapter);

    dt_remote_patch_t patch;
    vector_patch_init(&patch);
    g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", boundary_values[i], 3));

    void *projected = g_malloc(fixture->module->params_size);
    dt_remote_error_t *error = NULL;
    assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
    assert_null(error);
    assert_float_equal(read_color_component(fixture, projected, "color", 0),
                       expected_first_component[i], 0.0);

    g_free(projected);
    vector_patch_cleanup(&patch);
    borders_fixture_free(fixture);
  }
}

static void test_apply_patch_domain_violation_rejects_and_leaves_live_params_untouched(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  const double values[] = { 0.1, 1.00000001, 0.2 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "domain"));
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_nan_rejection_preserves_params_and_input(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  const double raw_values[] = { 0.1, NAN, 0.2 };
  dt_remote_semantic_patch_t *semantic =
    make_vector_patch("vector.color", raw_values, G_N_ELEMENTS(raw_values));
  const gpointer input_data = semantic->value.vector.values->data;
  const guint input_length = semantic->value.vector.values->len;
  GArray *input_before = make_values(raw_values, G_N_ELEMENTS(raw_values));
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, semantic);

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_vector_error_details(error, "vector.color", 1, "non_finite");
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);
  assert_int_equal(semantic->value.vector.values->len, input_length);
  assert_ptr_equal(semantic->value.vector.values->data, input_data);
  assert_memory_equal(semantic->value.vector.values->data, input_before->data,
                      input_before->len * sizeof(double));

  dt_remote_error_free(error);
  g_array_unref(input_before);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

// VEC3-014: dt_remote_vector_validate() proves LEVELS ordering/minimum_gap
// only in double precision (called at the top of write_vector_patch,
// remote_vector.c); two double-domain-distinct, strictly increasing values
// can still collapse to the same float32 once narrowed for the actual
// native write. minimum_gap == 0 (itself a valid LEVELS descriptor per
// vector_subtype_metadata_is_valid()) makes this reachable with an
// otherwise unremarkable adjacent pair.
static dt_remote_vector_descriptor_t make_levels_apply_descriptor(double minimum_gap)
{
  dt_remote_vector_descriptor_t desc = make_color_descriptor();
  desc.name = "vector.levels";
  desc.subtype = DT_REMOTE_VECTOR_LEVELS;
  desc.strictly_increasing = TRUE;
  desc.minimum_gap = minimum_gap;
  return desc;
}

static void test_apply_patch_levels_rejects_values_that_collapse_after_narrowing(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_levels_apply_descriptor(0.0);
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  // Strictly increasing in double precision (delta ~1e-10 > 0, satisfying
  // both ordering and the minimum_gap == 0 threshold), but the first two
  // values narrow to the identical float32 -- collapsing the ordering the
  // double-domain validator just proved.
  const double values[] = { 0.5, 0.5 + 1e-10, 0.9 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.levels", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_levels_accepts_values_that_remain_ordered_after_narrowing(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_levels_apply_descriptor(0.0);
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  const double values[] = { 0.1, 0.5, 0.9 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.levels", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);
  assert_float_equal(read_color_component(fixture, projected, "color", 0), 0.1f, 1e-6);
  assert_float_equal(read_color_component(fixture, projected, "color", 1), 0.5f, 1e-6);
  assert_float_equal(read_color_component(fixture, projected, "color", 2), 0.9f, 1e-6);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_invalid_native_layout_preserves_params(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.component_count = G_N_ELEMENTS(s_rgba_components);
  descriptor.components = s_rgba_components;
  descriptor.native_capacity = 3;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  const double values[] = { 0.1, 0.2, 0.3, 0.4 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values,
                  make_vector_patch("vector.color", values, G_N_ELEMENTS(values)));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_succeeds_and_writes_native_floats(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();

  static const double values[] = { 0.11, 0.22, 0.33 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  assert_float_equal(read_color_component(fixture, projected, "color", 0), 0.11f, 1e-6);
  assert_float_equal(read_color_component(fixture, projected, "color", 1), 0.22f, 1e-6);
  assert_float_equal(read_color_component(fixture, projected, "color", 2), 0.33f, 1e-6);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_narrowing_write_preserves_tail_and_every_other_byte(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.name = "vector.color_partial";
  descriptor.component_count = 2;
  descriptor.components = s_rg_components;
  descriptor.native_capacity = 3;
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
  };
  install_vector_adapter(&adapter);

  // Seed a sentinel into the preserved tail (blue) and build the expected
  // post-write buffer by hand, so the assertion below is a full-byte
  // memcmp -- not just "the two written floats look right".
  dt_introspection_field_t *color_field = NULL;
  float *color_ptr = dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                                fixture->module->params, "color", &color_field);
  assert_non_null(color_ptr);
  color_ptr[2] = 0.42f;

  static const double values[] = { 0.61, 0.62 };

  void *expected = g_malloc(fixture->module->params_size);
  memcpy(expected, fixture->module->params, fixture->module->params_size);
  dt_introspection_field_t *expected_color_field = NULL;
  float *expected_color_ptr = dt_introspection_get_child(
    fixture->module->so->get_introspection()->field, expected, "color", &expected_color_field);
  // Derive the expected float bit pattern from the same double->float cast
  // the production write path performs, so this is a genuine full-byte
  // memcmp and not vulnerable to an independent double-rounding mismatch
  // against hand-written float literals.
  expected_color_ptr[0] = (float)values[0];
  expected_color_ptr[1] = (float)values[1];
  // expected_color_ptr[2] left at the sentinel 0.42f: never touched.

  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color_partial", values, 2));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  assert_memory_equal(projected, expected, fixture->module->params_size);

  g_free(projected);
  g_free(expected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_writable_when_evaluated_after_scalar_prepare_writes(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static const dt_remote_parameter_predicate_t predicate = {
    .field = "aspect_orient", .op = DT_REMOTE_PREDICATE_EQ,
    .enum_name = "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT"
  };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  descriptor.active_when = NULL;
  descriptor.writable_when = &predicate;
  static const char *const prepare_fields[] = { "aspect_orient" };
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
    .prepare_fields = prepare_fields, .prepare_field_count = 1,
  };
  install_vector_adapter(&adapter);

  // default aspect_orient is AUTO: the predicate is false, so a vector
  // write with no accompanying scalar change must be rejected.
  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  dt_remote_error_free(error);
  g_free(projected);
  vector_patch_cleanup(&patch);

  // Same patch, but with a scalar entry flipping aspect_orient to PORTRAIT
  // in the same request: the predicate must be evaluated against the
  // post-scalar-write projected block, so this now succeeds.
  vector_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  make_orient_entry(fixture, "DT_IOP_BORDERS_ASPECT_ORIENTATION_PORTRAIT"));
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  projected = g_malloc(fixture->module->params_size);
  error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

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

static void test_apply_patch_scalar_only_prepare_field_still_triggers_validate_completed(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();

  static const char *const prepare_fields[] = { "aspect_orient" };
  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
    .prepare_fields = prepare_fields, .prepare_field_count = 1,
    .validate_completed = test_validate_completed,
  };
  install_vector_adapter(&adapter);
  s_validate_completed_calls = 0;
  s_validate_completed_result = TRUE;

  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values,
                  make_orient_entry(fixture, "DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE"));
  // No vector semantic entries at all -- only the prepare-field scalar
  // write should be enough to trigger validate_completed().

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);
  assert_int_equal(s_validate_completed_calls, 1);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

// VEC3-011: a malformed adapter (prepare_field_count > 0 but prepare_fields
// == NULL -- exactly the shape dt_remote_vector_registry_validate()'s own
// envelope check rejects) must not crash patch_mentions_prepare_field()'s
// traversal when a scalar-only patch (no vector semantic entries, so
// has_vector_semantics is FALSE and the short-circuit that would otherwise
// skip straight to full registry validation does not apply) is applied.
// The scalar entry below re-asserts aspect_orient's own current value (its
// enum name read back from the live params before building the patch), so
// applying it alone is a byte-for-byte no-op -- isolating the assertion to
// whether the vector engine itself ever wrote anything, same convention as
// the other atomicity tests in this file.
static void test_apply_patch_malformed_prepare_fields_envelope_fails_closed(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
    .prepare_field_count = 1, .prepare_fields = NULL, // malformed: count > 0, array NULL
  };
  install_vector_adapter(&adapter);

  dt_introspection_field_t *orient_field = NULL;
  int *orient_ptr = dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                               fixture->module->params, "aspect_orient", &orient_field);
  assert_non_null(orient_ptr);
  const char *current_orient_name = dt_introspection_get_enum_name(orient_field, *orient_ptr);
  assert_non_null(current_orient_name);

  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values, make_orient_entry(fixture, current_orient_name));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);
  assert_memory_equal(projected, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_validate_completed_rejection_rolls_back(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  static dt_remote_vector_descriptor_t descriptor;
  descriptor = make_color_descriptor();
  static dt_remote_vector_module_adapter_t adapter;
  adapter = (dt_remote_vector_module_adapter_t){
    .operation = "borders", .minimum_params_version = 4, .maximum_params_version = 4,
    .vectors = &descriptor, .vector_count = 1,
    .validate_completed = test_validate_completed,
  };
  install_vector_adapter(&adapter);
  s_validate_completed_calls = 0;
  s_validate_completed_result = FALSE;

  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  assert_int_equal(s_validate_completed_calls, 1);
  // The caller only ever committed to `projected`, never to the live
  // params -- same rollback contract as the curve engine (the caller
  // applies to temp_params and discards it on failure).
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

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

static void test_apply_patch_skips_non_vector_semantic_entries(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();

  static const double values[] = { 0.1, 0.2, 0.3 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  // A curve-class entry for an ID this engine has never heard of: it must
  // be skipped rather than reported as an unknown field.
  g_ptr_array_add(patch.semantic_values, make_curve_class_patch("curve.unrelated"));
  g_ptr_array_add(patch.semantic_values, make_vector_patch("vector.color", values, 3));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);
  assert_float_equal(read_color_component(fixture, projected, "color", 0), 0.1f, 1e-6);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_apply_patch_no_vector_content_returns_true_without_touching_registry(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  dt_remote_vector_registry_set_lookup_override(always_null_vector_lookup);

  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values, make_orient_entry(fixture, "DT_IOP_BORDERS_ASPECT_ORIENTATION_LANDSCAPE"));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* colorbalance adapter (production registry; milestone4 vector-class      */
/* design doc SS Initial registry mapping / colorbalance). Unlike every     */
/* test above, these exercise the *production* s_adapters[] table (Task 5) */
/* through the public engine API directly -- no dt_remote_vector_registry_ */
/* set_lookup_override() install -- the same "real adapter, no override"   */
/* pattern test_remote_curve_registry.c uses for rgbcurve/tonecurve/       */
/* colorzones/basecurve once those ship in the curve registry. lift/gamma/ */
/* gain and offset/power/slope are six mode-gated aliases over the same    */
/* three lift[4]/gamma[4]/gain[4] float arrays (colorbalance.c:100);       */
/* `mode` (colorbalance.c:59) selects which alias set is live, defaulting  */
/* to SLOPE_OFFSET_POWER (offset/power/slope).                             */
/* ---------------------------------------------------------------------- */

static dt_remote_patch_entry_t *make_colorbalance_mode_entry(const borders_fixture_t *fixture,
                                                              const char *enum_name)
{
  dt_introspection_field_t *field = NULL;
  (void)dt_introspection_get_child(fixture->module->so->get_introspection()->field,
                                   fixture->module->params, "mode", &field);
  int value = 0;
  assert_true(dt_introspection_get_enum_value(field, enum_name, &value));

  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup("mode");
  entry->value.type = DT_REMOTE_VALUE_ENUM;
  entry->value.v.e.value = value;
  entry->value.v.e.name = g_strdup(enum_name);
  return entry;
}

static void test_colorbalance_schema_lists_six_mode_gated_aliases_in_order(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("colorbalance");
  assert_non_null(so);

  GPtrArray *schemas = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_vector_list_schema(so, &schemas, &error));
  assert_null(error);
  assert_non_null(schemas);
  assert_int_equal(schemas->len, 6);

  static const char *const expected_names[] = { "lift", "gamma", "gain", "offset", "power", "slope" };
  // lift/gamma/gain are writable when mode != SLOPE_OFFSET_POWER (NE);
  // offset/power/slope alias the same three arrays, writable when
  // mode == SLOPE_OFFSET_POWER (EQ) -- the module's default mode.
  static const dt_remote_predicate_operator_t expected_ops[] = {
    DT_REMOTE_PREDICATE_NE, DT_REMOTE_PREDICATE_NE, DT_REMOTE_PREDICATE_NE,
    DT_REMOTE_PREDICATE_EQ, DT_REMOTE_PREDICATE_EQ, DT_REMOTE_PREDICATE_EQ,
  };
  static const char *const expected_component_names[] = { "factor", "red", "green", "blue" };

  for(guint i = 0; i < 6; i++)
  {
    const dt_remote_vector_schema_t *schema = g_ptr_array_index(schemas, i);
    assert_string_equal(schema->name, expected_names[i]);
    assert_int_equal(schema->subtype, DT_REMOTE_VECTOR_PLAIN);
    assert_null(schema->color_space);
    assert_int_equal(schema->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
    assert_non_null(schema->writable_when);
    assert_string_equal(schema->writable_when->field, "mode");
    assert_string_equal(schema->writable_when->enum_name, "SLOPE_OFFSET_POWER");
    assert_int_equal(schema->writable_when->op, expected_ops[i]);
    assert_non_null(schema->active_when);
    assert_string_equal(schema->active_when->field, "mode");
    assert_string_equal(schema->active_when->enum_name, "SLOPE_OFFSET_POWER");
    assert_int_equal(schema->active_when->op, expected_ops[i]);

    assert_int_equal(schema->components->len, 4);
    for(guint c = 0; c < 4; c++)
    {
      const dt_remote_vector_component_schema_t *component =
        &g_array_index(schema->components, dt_remote_vector_component_schema_t, c);
      assert_string_equal(component->name, expected_component_names[c]);
      assert_float_equal(component->minimum, 0.0, 0.0);
      assert_float_equal(component->maximum, 2.0, 0.0);
    }
  }

  g_ptr_array_unref(schemas);
}

// The component-order proof, against an *independently constructed* blob:
// the params blob comes from the module's own introspection defaults, and
// the poke below writes directly through dt_introspection_access_array()
// (the same primitive write_color_component() wraps) -- never through
// dt_remote_vector_apply_patch() or any other part of the engine under
// test. A component-order bug that swapped the write and read paths
// identically would still pass a write-then-readback round trip; poking
// the native offset independently and reading it back through
// dt_remote_vector_read_values() is what actually proves component 1 of
// the "lift" descriptor is red, not e.g. green or factor.
static void test_colorbalance_read_values_proves_component_order_against_independent_blob(void **state)
{
  (void)state;
  borders_fixture_t *fixture = real_vector_module_fixture_new("colorbalance");

  write_color_component(fixture, fixture->module->params, "lift", 1, 1.25f);

  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &error));
  assert_null(error);
  dt_remote_vector_value_t *lift = g_hash_table_lookup(values, "lift");
  assert_non_null(lift);
  assert_int_equal(lift->values->len, 4);
  // component 0 ("factor"), 2 ("green"), 3 ("blue") are untouched
  // defaults; only component 1 ("red") was poked.
  assert_float_equal(g_array_index(lift->values, double, 0), 1.0, 1e-6);
  assert_float_equal(g_array_index(lift->values, double, 1), 1.25, 1e-6);
  assert_float_equal(g_array_index(lift->values, double, 2), 1.0, 1e-6);
  assert_float_equal(g_array_index(lift->values, double, 3), 1.0, 1e-6);

  g_hash_table_unref(values);
  borders_fixture_free(fixture);
}

static void test_colorbalance_default_mode_gates_offset_writable_lift_not(void **state)
{
  (void)state;
  borders_fixture_t *fixture = real_vector_module_fixture_new("colorbalance");
  // Module default is SLOPE_OFFSET_POWER (colorbalance.c:62); no scalar
  // write needed to reach the state under test.

  GHashTable *values = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_vector_read_values(fixture->module, fixture->module->params, &values, &error));
  assert_null(error);

  static const char *const lgg_names[] = { "lift", "gamma", "gain" };
  static const char *const sop_names[] = { "offset", "power", "slope" };
  for(guint i = 0; i < G_N_ELEMENTS(lgg_names); i++)
  {
    dt_remote_vector_value_t *value = g_hash_table_lookup(values, lgg_names[i]);
    assert_non_null(value);
    assert_false(value->active);
    assert_false(value->writable_now);
  }
  for(guint i = 0; i < G_N_ELEMENTS(sop_names); i++)
  {
    dt_remote_vector_value_t *value = g_hash_table_lookup(values, sop_names[i]);
    assert_non_null(value);
    assert_true(value->active);
    assert_true(value->writable_now);
  }

  g_hash_table_unref(values);
  borders_fixture_free(fixture);
}

static void test_colorbalance_apply_patch_rejects_lift_write_under_default_sop_mode(void **state)
{
  (void)state;
  borders_fixture_t *fixture = real_vector_module_fixture_new("colorbalance");
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  static const double lift_values[] = { 1.1, 1.2, 1.3, 1.4 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("lift", lift_values, 4));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_false(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_non_null(error);
  // Same not-writable error shape the curve engine returns for a gated-off
  // write (see e.g. test_apply_patch_active_when_is_evaluated_independently
  // above and the colorzones tests in test_remote_curve.c): unsupported
  // field, not invalid value.
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);
  assert_memory_equal(projected, before, fixture->module->params_size);
  assert_memory_equal(fixture->module->params, before, fixture->module->params_size);

  dt_remote_error_free(error);
  g_free(projected);
  g_free(before);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_colorbalance_composed_mode_switch_writes_lift_leaves_gamma_gain_untouched(void **state)
{
  (void)state;
  borders_fixture_t *fixture = real_vector_module_fixture_new("colorbalance");

  static const double lift_values[] = { 0.4, 0.5, 0.6, 0.7 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.scalar_values, make_colorbalance_mode_entry(fixture, "LIFT_GAMMA_GAIN"));
  g_ptr_array_add(patch.semantic_values, make_vector_patch("lift", lift_values, 4));

  void *projected = g_malloc(fixture->module->params_size);
  dt_remote_error_t *error = NULL;
  assert_true(vector_apply_to_copy(fixture, &patch, projected, &error));
  assert_null(error);

  for(guint i = 0; i < 4; i++)
    assert_float_equal(read_color_component(fixture, projected, "lift", i), (float)lift_values[i], 1e-6);
  // gamma/gain are separate native arrays from lift's; writing "lift" must
  // never touch them, mode switch or not.
  for(guint i = 0; i < 4; i++)
  {
    assert_float_equal(read_color_component(fixture, projected, "gamma", i),
                       read_color_component(fixture, fixture->module->params, "gamma", i), 0.0);
    assert_float_equal(read_color_component(fixture, projected, "gain", i),
                       read_color_component(fixture, fixture->module->params, "gain", i), 0.0);
  }

  g_free(projected);
  vector_patch_cleanup(&patch);
  borders_fixture_free(fixture);
}

static void test_colorbalance_alias_write_conflict_is_rejected_atomically(void **state)
{
  (void)state;
  borders_fixture_t *fixture = real_vector_module_fixture_new("colorbalance");
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  // Default mode is SLOPE_OFFSET_POWER: "offset" is writable, "lift" is
  // not -- gating makes it structurally impossible for both aliases of
  // one array to be writable at once, under any mode. "lift" sorts first
  // in registry order, so it is the descriptor evaluated (and rejected)
  // before "offset" is ever reached.
  static const double lift_values[] = { 1.1, 1.2, 1.3, 1.4 };
  static const double offset_values[] = { 0.4, 0.5, 0.6, 0.7 };
  dt_remote_patch_t patch;
  vector_patch_init(&patch);
  g_ptr_array_add(patch.semantic_values, make_vector_patch("offset", offset_values, 4));
  g_ptr_array_add(patch.semantic_values, make_vector_patch("lift", lift_values, 4));

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

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_vector_schema_free_fully_populated),
    cmocka_unit_test(test_vector_schema_free_partially_populated),
    cmocka_unit_test(test_vector_schema_free_null_is_safe),
    cmocka_unit_test(test_vector_value_free_fully_populated),
    cmocka_unit_test(test_vector_value_free_partially_populated),
    cmocka_unit_test(test_vector_value_free_null_is_safe),

    cmocka_unit_test(test_vector_validate_accepts_well_formed_values),
    cmocka_unit_test(test_vector_validate_rejects_count_mismatch),
    cmocka_unit_test(test_vector_validate_rejects_component_below_minimum),
    cmocka_unit_test(test_vector_validate_rejects_component_above_maximum_at_double_boundary),
    cmocka_unit_test(test_vector_validate_accepts_component_exactly_at_maximum),
    cmocka_unit_test(test_vector_validate_rejects_non_finite_components),
    cmocka_unit_test(test_vector_validate_levels_rejects_nan_in_every_position),
    cmocka_unit_test(test_vector_validate_accepts_exact_finite_minima),
    cmocka_unit_test(test_vector_validate_never_mutates_values_on_rejection),
    cmocka_unit_test(test_vector_validate_rejects_candidate_exceeding_native_float_range),
    cmocka_unit_test(test_vector_validate_rejects_negative_candidate_exceeding_native_float_range),
    cmocka_unit_test(test_vector_validate_accepts_candidate_exactly_at_native_float_range_boundary),
    cmocka_unit_test(
      test_vector_validate_accepts_candidate_exactly_at_negative_native_float_range_boundary),
    cmocka_unit_test(test_vector_validate_levels_accepts_strictly_increasing),
    cmocka_unit_test(test_vector_validate_levels_rejects_unordered),
    cmocka_unit_test(test_vector_validate_levels_rejects_equal_adjacent_values),
    cmocka_unit_test(test_vector_validate_levels_rejects_gap_below_flt_epsilon),
    cmocka_unit_test(test_vector_validate_levels_accepts_gap_exactly_at_flt_epsilon),

    cmocka_unit_test(test_registry_validate_rejects_invalid_adapter_envelope),
    cmocka_unit_test(test_registry_validate_rejects_invalid_descriptor_metadata),
    cmocka_unit_test(test_registry_validate_accepts_component_bounds_at_native_float_range_boundary),
    cmocka_unit_test(test_registry_validate_rejects_invalid_subtype_metadata),
    cmocka_unit_test(test_registry_validate_rejects_color_with_nonzero_minimum_gap),
    cmocka_unit_test(test_registry_validate_accepts_valid_color_and_levels_metadata),
    cmocka_unit_test(test_registry_validate_isolates_native_layout_failures),
    cmocka_unit_test(test_registry_validate_rejects_missing_float_element_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_nonfloat_element_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_wrong_float_element_size),
    cmocka_unit_test(test_registry_validate_rejects_oversized_native_count),
    cmocka_unit_test(test_registry_validate_rejects_undersized_aggregate_array_byte_size),
    cmocka_unit_test(test_registry_validate_passes_for_well_formed_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_native_capacity_mismatch),
    cmocka_unit_test(test_registry_validate_rejects_duplicate_names_within_adapter),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_name_colliding_with_curve_registry,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_registry_validate_rechecks_curve_collision_after_cached_success,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_registry_validate_does_not_cache_curve_collision,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_registry_validate_retains_intrinsic_cached_result,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test(test_registry_validate_rejects_color_without_color_space),
    cmocka_unit_test(test_registry_validate_accepts_color_with_color_space),
    cmocka_unit_test(test_registry_validate_rejects_levels_without_strictly_increasing),

    cmocka_unit_test_setup_teardown(test_list_schema_no_adapter_yields_null_and_succeeds,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_invalid_native_layout_clears_output,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_converts_descriptors_in_registry_order,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_read_values_widens_floats_to_doubles,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_stamps_active_and_writable_now_from_predicate,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_read_values_evaluates_active_and_writable_predicates_independently,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_read_values_reads_multiple_descriptors_independently,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_no_adapter_returns_empty_table,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_invalid_native_layout_clears_output,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_apply_patch_unknown_id_fails_with_unknown_field,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_no_adapter_for_op_fails_with_unknown_field,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_duplicate_id_fails_with_invalid_value,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_count_mismatch_fails_with_invalid_value,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_rejects_bound_and_candidate_beyond_native_float_range,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_accepts_candidate_exactly_at_native_float_range_boundary,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_domain_violation_rejects_and_leaves_live_params_untouched,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_nan_rejection_preserves_params_and_input,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_levels_rejects_values_that_collapse_after_narrowing,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_levels_accepts_values_that_remain_ordered_after_narrowing,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_invalid_native_layout_preserves_params,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_succeeds_and_writes_native_floats,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_narrowing_write_preserves_tail_and_every_other_byte,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_writable_when_evaluated_after_scalar_prepare_writes,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_active_when_is_evaluated_independently,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_scalar_only_prepare_field_still_triggers_validate_completed,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_malformed_prepare_fields_envelope_fails_closed,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_validate_completed_rejection_rolls_back,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_validate_completed_observes_all_writes,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_uses_registry_order_for_aliases,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_skips_non_vector_semantic_entries,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_no_vector_content_returns_true_without_touching_registry,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_colorbalance_schema_lists_six_mode_gated_aliases_in_order,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_colorbalance_read_values_proves_component_order_against_independent_blob,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_colorbalance_default_mode_gates_offset_writable_lift_not,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_colorbalance_apply_patch_rejects_lift_write_under_default_sop_mode,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_colorbalance_composed_mode_switch_writes_lift_leaves_gamma_gain_untouched,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_colorbalance_alias_write_conflict_is_rejected_atomically,
                                    lookup_override_test_setup, lookup_override_test_teardown),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
