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

static borders_fixture_t *borders_fixture_new(void)
{
  borders_fixture_t *fixture = g_new0(borders_fixture_t, 1);
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

static const dt_remote_vector_module_adapter_t *s_lookup_override_adapter = NULL;

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
  assert_registry_rejects_private_color_array(FALSE, DT_INTROSPECTION_TYPE_FLOAT, 3);
}

static void test_registry_validate_rejects_nonfloat_element_descriptor(void **state)
{
  (void)state;
  assert_registry_rejects_private_color_array(TRUE, DT_INTROSPECTION_TYPE_CHAR, 3);
}

static void test_registry_validate_rejects_oversized_native_count(void **state)
{
  (void)state;
  if(sizeof(size_t) <= sizeof(guint)) return;

  const size_t oversized_count =
    (size_t)G_MAXUINT + 1 + G_N_ELEMENTS(s_rgb_components);
  assert_registry_rejects_private_color_array(TRUE, DT_INTROSPECTION_TYPE_FLOAT,
                                              oversized_count);
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
  // No override installed by lookup_override_test_setup(): the empty
  // production table applies.

  GPtrArray *fields = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_list_schema(so, &fields, &err));
  assert_null(err);
  assert_null(fields);
}

static void test_list_schema_converts_descriptors_in_registry_order(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);

  static dt_remote_vector_descriptor_t descriptors[2];
  descriptors[0] = make_color_descriptor();
  descriptors[0].name = "vector.first";
  descriptors[1] = make_color_descriptor();
  descriptors[1].name = "vector.second";
  descriptors[1].native = s_frame_color_path;
  descriptors[1].subtype = DT_REMOTE_VECTOR_COLOR;
  descriptors[1].color_space = "display_rgb";
  descriptors[1].writable_when = NULL;
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
  assert_string_equal(first->name, "vector.first");
  assert_string_equal(second->name, "vector.second");
  assert_int_equal(first->subtype, DT_REMOTE_VECTOR_PLAIN);
  assert_null(first->color_space);
  assert_int_equal(first->components->len, 3);
  assert_string_equal(
    g_array_index(first->components, dt_remote_vector_component_schema_t, 0).name, "red");
  assert_int_equal(second->subtype, DT_REMOTE_VECTOR_COLOR);
  assert_string_equal(second->color_space, "display_rgb");
  assert_int_equal(first->writability, DT_REMOTE_WRITABLE_NOW);

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

static void test_read_values_no_adapter_returns_empty_table(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("borders");
  assert_non_null(so);
  dt_iop_module_t module;
  init_fake_module(&module, so);

  void *blob = g_malloc0(so->get_introspection()->size);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_vector_read_values(&module, blob, &values, &err));
  assert_null(err);
  assert_non_null(values);
  assert_int_equal(g_hash_table_size(values), 0);

  g_hash_table_unref(values);
  g_free(blob);
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
  // No override installed: the empty production table applies, so "borders"
  // has no vector adapter at all.

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

static void test_apply_patch_domain_violation_rejects_and_leaves_live_params_untouched(void **state)
{
  (void)state;
  borders_fixture_t *fixture = borders_fixture_new();
  install_simple_vector_adapter();
  void *before = g_malloc(fixture->module->params_size);
  memcpy(before, fixture->module->params, fixture->module->params_size);

  static const double values[] = { 0.1, 1.5, 0.2 }; // green out of [0,1]
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
  descriptor.active_when = &predicate;
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
  // No override installed and no prepare-field scalar entries: this patch
  // must succeed trivially without consulting the (empty) vector registry
  // at all -- mirrors the curve engine's own "scalar-only patch unrelated
  // to adapter preparation" contract.

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
    cmocka_unit_test(test_vector_validate_levels_accepts_strictly_increasing),
    cmocka_unit_test(test_vector_validate_levels_rejects_unordered),
    cmocka_unit_test(test_vector_validate_levels_rejects_equal_adjacent_values),
    cmocka_unit_test(test_vector_validate_levels_rejects_gap_below_flt_epsilon),
    cmocka_unit_test(test_vector_validate_levels_accepts_gap_exactly_at_flt_epsilon),

    cmocka_unit_test(test_registry_validate_rejects_invalid_adapter_envelope),
    cmocka_unit_test(test_registry_validate_rejects_invalid_descriptor_metadata),
    cmocka_unit_test(test_registry_validate_rejects_invalid_subtype_metadata),
    cmocka_unit_test(test_registry_validate_rejects_color_with_nonzero_minimum_gap),
    cmocka_unit_test(test_registry_validate_accepts_valid_color_and_levels_metadata),
    cmocka_unit_test(test_registry_validate_isolates_native_layout_failures),
    cmocka_unit_test(test_registry_validate_rejects_missing_float_element_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_nonfloat_element_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_oversized_native_count),
    cmocka_unit_test(test_registry_validate_passes_for_well_formed_descriptor),
    cmocka_unit_test(test_registry_validate_rejects_native_capacity_mismatch),
    cmocka_unit_test(test_registry_validate_rejects_duplicate_names_within_adapter),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_name_colliding_with_curve_registry,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test(test_registry_validate_rejects_color_without_color_space),
    cmocka_unit_test(test_registry_validate_accepts_color_with_color_space),
    cmocka_unit_test(test_registry_validate_rejects_levels_without_strictly_increasing),

    cmocka_unit_test_setup_teardown(test_list_schema_no_adapter_yields_null_and_succeeds,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_converts_descriptors_in_registry_order,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_read_values_widens_floats_to_doubles,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_stamps_active_and_writable_now_from_predicate,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_no_adapter_returns_empty_table,
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
      test_apply_patch_domain_violation_rejects_and_leaves_live_params_untouched,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_succeeds_and_writes_native_floats,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_narrowing_write_preserves_tail_and_every_other_byte,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_writable_when_evaluated_after_scalar_prepare_writes,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_scalar_only_prepare_field_still_triggers_validate_completed,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_validate_completed_rejection_rolls_back,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_skips_non_vector_semantic_entries,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_no_vector_content_returns_true_without_touching_registry,
                                    lookup_override_test_setup, lookup_override_test_teardown),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
