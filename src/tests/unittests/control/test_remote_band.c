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
 * cmocka unit tests for the band-class engine and registry core
 * (src/control/remote_band.c/.h, src/control/remote_band_registry.c),
 * mirroring test_remote_vector.c's structure and fixture idioms for the
 * vector twin. The production adapter table (remote_band_registry.c's
 * s_adapters[]) is empty in this task -- every behavior below is proven
 * with a test-local, hand-rolled adapter/descriptor against real loaded
 * module .so's, installed through dt_remote_band_registry_set_lookup_override()
 * (the same test seam dt_remote_vector_registry_set_lookup_override()
 * provides for the vector registry).
 *
 * Two real fixture ops, per the task brief:
 *  - "lowlight" (dt_iop_lowlight_params_t: `blueness`, `transition_x[6]`,
 *    `transition_y[6]`) for the generic engine tests, wrapped in a single
 *    hand-rolled FIXED-policy descriptor ("band.transition").
 *  - "atrous" (dt_iop_atrous_params_t: `octaves`, `x[5][6]`, `y[5][6]`,
 *    `mix`) for the x-policy/twin tests, wrapped in a hand-rolled
 *    INTERIOR-policy descriptor pair over channel rows 0/1
 *    ("band.twin_a"/"band.twin_b") sharing x via x_shared_with.
 *
 * Neither fixture module has an enum-typed params field, so the
 * active_when/writable_when predicate *matching* branches (proven
 * exhaustively for the identical shared code path in test_remote_vector.c)
 * are not independently re-proven here beyond the NULL-predicate
 * (unconditional) case and the predicate-drift (unresolvable field)
 * failure path -- see the read_values section below for the explicit note.
 *
 * Coverage:
 *  - dt_remote_band_schema_free()/dt_remote_band_value_free() on fully
 *    populated, partially populated, and NULL instances.
 *  - dt_remote_band_validate(): pure, y count/finiteness/domain (including
 *    the double-precision boundary), x under INTERIOR (count, endpoint
 *    pinning, ordering, minimum-gap), x ignored under FIXED.
 *  - dt_remote_band_registry_validate(): native path shape (exact count,
 *    not "at least"), duplicate IDs, cross-class collisions with both the
 *    curve and vector registries, x_shared_with symmetry, x-policy
 *    minimum_gap rules.
 *  - dt_remote_band_list_schema(): descriptor -> owned schema conversion
 *    in registry order; no-adapter silent degrade (NULL, not empty array).
 *  - dt_remote_band_read_values(): float->double widening for both y and
 *    x; active/writable_now stamped TRUE for NULL predicates.
 *  - dt_remote_band_apply_patch()/_apply_entries(): unknown ID, duplicate
 *    ID, y length mismatch, x-under-FIXED rejection, domain rejection
 *    (with the projected block left untouched), a successful write that
 *    leaves every other byte untouched, the prepare-field gate triggering
 *    validate_completed on a scalar-only patch, twin-conflict detection,
 *    twin x-sync propagation, endpoint-pinning rejection, validate_completed
 *    observing the post-write state and its rejection propagating, and a
 *    mixed-class patch silently skipping a non-bands entry.
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

#include "control/remote_band.h"
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
/* dt_remote_band_schema_free                                              */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *make_condition(const char *field, const char *enum_name)
{
  dt_remote_parameter_condition_t *c = g_new0(dt_remote_parameter_condition_t, 1);
  c->field = g_strdup(field);
  c->op = DT_REMOTE_PREDICATE_EQ;
  c->enum_name = g_strdup(enum_name);
  return c;
}

static GArray *make_values(const double *values, guint n)
{
  GArray *array = g_array_sized_new(FALSE, FALSE, sizeof(double), n);
  if(n) g_array_append_vals(array, values, n);
  return array;
}

static void test_band_schema_free_fully_populated(void **state)
{
  (void)state;
  static const double x[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
  dt_remote_band_schema_t *schema = g_new0(dt_remote_band_schema_t, 1);
  schema->name = g_strdup("bands.luma");
  schema->display_name = g_strdup("Luma");
  schema->description = g_strdup("luminance bands");
  schema->count = 6;
  schema->y_minimum = 0.0;
  schema->y_maximum = 1.0;
  schema->x_policy = DT_REMOTE_BAND_X_INTERIOR;
  schema->minimum_gap = 0.001;
  schema->x_shared_with = g_strdup("bands.luma_threshold");
  schema->x = make_values(x, G_N_ELEMENTS(x));
  schema->writability = DT_REMOTE_WRITABLE_CONDITIONAL;
  schema->active_when = make_condition("mode", "MODE_ON");
  schema->writable_when = make_condition("mode", "MODE_ON");

  dt_remote_band_schema_free(schema);
  // no crash/double-free is the assertion; cmocka has nothing else to check
}

static void test_band_schema_free_partially_populated(void **state)
{
  (void)state;
  // description, x_shared_with, x, and the two conditions are all
  // documented nullable; leave them unset to exercise the NULL branches.
  dt_remote_band_schema_t *schema = g_new0(dt_remote_band_schema_t, 1);
  schema->name = g_strdup("bands.luma");
  schema->display_name = g_strdup("Luma");
  schema->writability = DT_REMOTE_WRITABLE_NOW;

  dt_remote_band_schema_free(schema);
}

static void test_band_schema_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_band_schema_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_value_free                                               */
/* ---------------------------------------------------------------------- */

static void test_band_value_free_fully_populated(void **state)
{
  (void)state;
  static const double y[] = { 0.1, 0.2, 0.3 };
  static const double x[] = { 0.0, 0.5, 1.0 };
  dt_remote_band_value_t *value = g_new0(dt_remote_band_value_t, 1);
  value->name = g_strdup("bands.luma");
  value->y = make_values(y, G_N_ELEMENTS(y));
  value->x = make_values(x, G_N_ELEMENTS(x));
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;

  dt_remote_band_value_free(value);
}

static void test_band_value_free_partially_populated(void **state)
{
  (void)state;
  dt_remote_band_value_t *value = g_new0(dt_remote_band_value_t, 1);
  value->name = g_strdup("bands.luma");

  dt_remote_band_value_free(value);
}

static void test_band_value_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_band_value_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_validate                                                 */
/* ---------------------------------------------------------------------- */

/*
 * dt_remote_band_validate() is pure (no introspection, no registry, no
 * JSON parsing of the input), so these tests build dt_remote_band_descriptor_t
 * instances by hand -- no live module or dt_init() dependency.
 */

static dt_remote_band_descriptor_t make_pure_fixed_descriptor(void)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.pure_fixed";
  desc.count = 4;
  desc.y_minimum = 0.0;
  desc.y_maximum = 1.0;
  desc.x_policy = DT_REMOTE_BAND_X_FIXED;
  desc.minimum_gap = 0.0;
  return desc;
}

static dt_remote_band_descriptor_t make_pure_interior_descriptor(double minimum_gap)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.pure_interior";
  desc.count = 4;
  desc.y_minimum = 0.0;
  desc.y_maximum = 1.0;
  desc.x_policy = DT_REMOTE_BAND_X_INTERIOR;
  desc.minimum_gap = minimum_gap;
  return desc;
}

static dt_remote_band_descriptor_t make_pure_wide_bound_descriptor(void)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.pure_wide";
  desc.count = 3;
  desc.y_minimum = -1e39;
  desc.y_maximum = 1e39;
  desc.x_policy = DT_REMOTE_BAND_X_FIXED;
  desc.minimum_gap = 0.0;
  return desc;
}

static const float s_pure_stored_x[4] = { 0.0f, 0.3f, 0.6f, 1.0f };

static void assert_band_error_details(dt_remote_error_t *err, const char *expected_parameter,
                                      const char *expected_array, int expected_index,
                                      const char *expected_constraint)
{
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(err->details_json);
  assert_non_null(strstr(err->details_json, expected_parameter));
  char array_needle[32];
  g_snprintf(array_needle, sizeof(array_needle), "\"array\":\"%s\"", expected_array);
  assert_non_null(strstr(err->details_json, array_needle));
  assert_non_null(strstr(err->details_json, expected_constraint));
  if(expected_index >= 0)
  {
    char needle[32];
    g_snprintf(needle, sizeof(needle), "\"index\":%d", expected_index);
    assert_non_null(strstr(err->details_json, needle));
  }
  else
  {
    assert_null(strstr(err->details_json, "\"index\""));
  }
}

static void test_band_validate_accepts_well_formed_y_only(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_null(err);

  g_array_unref(yv);
}

static void test_band_validate_rejects_y_count_mismatch(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.1, 0.5, 0.9 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_band_error_details(err, "band.pure_fixed", "y", -1, "count_mismatch");

  dt_remote_error_free(err);
  g_array_unref(yv);
}

static void test_band_validate_rejects_y_non_finite(void **state)
{
  (void)state;
  const double invalid[] = { NAN, INFINITY, -INFINITY };
  for(guint i = 0; i < G_N_ELEMENTS(invalid); i++)
  {
    dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
    const double raw[] = { 0.1, invalid[i], 0.5, 0.9 };
    GArray *yv = make_values(raw, G_N_ELEMENTS(raw));
    GArray *before = make_values(raw, G_N_ELEMENTS(raw));
    dt_remote_error_t *err = NULL;

    assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
    assert_band_error_details(err, "band.pure_fixed", "y", 1, "non_finite");
    assert_memory_equal(yv->data, before->data, yv->len * sizeof(double));

    dt_remote_error_free(err);
    g_array_unref(yv);
    g_array_unref(before);
  }
}

static void test_band_validate_rejects_y_below_minimum(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { -0.1, 0.5, 0.9, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_band_error_details(err, "band.pure_fixed", "y", 0, "domain");

  dt_remote_error_free(err);
  g_array_unref(yv);
}

// The reason y travels as doubles end-to-end: a value that would
// round-trip through float32 identically to the maximum must still be
// rejected when it is genuinely larger in double precision.
static void test_band_validate_rejects_y_above_maximum_at_double_boundary(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.1, 0.5, 0.9, 1.00000001 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_band_error_details(err, "band.pure_fixed", "y", 3, "domain");

  dt_remote_error_free(err);
  g_array_unref(yv);
}

static void test_band_validate_accepts_y_exactly_at_maximum(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.0, 0.5, 0.9, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_null(err);

  g_array_unref(yv);
}

static void test_band_validate_rejects_y_exceeding_native_float_range(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_wide_bound_descriptor();
  static const double y[] = { 1e39, 0.5, 0.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_band_error_details(err, "band.pure_wide", "y", 0, "native_range");

  dt_remote_error_free(err);
  g_array_unref(yv);
}

static void test_band_validate_accepts_y_exactly_at_native_float_range_boundary(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_wide_bound_descriptor();
  static const double y[] = { (double)FLT_MAX, -(double)FLT_MAX, 0.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_null(err);

  g_array_unref(yv);
}

static void test_band_validate_never_mutates_y_on_rejection(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.1, 3.0, 0.5, 0.9 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *snapshot = make_values(y, G_N_ELEMENTS(y));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, NULL, NULL, &err));
  assert_non_null(err);
  assert_memory_equal(yv->data, snapshot->data, yv->len * sizeof(double));

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(snapshot);
}

static void test_band_validate_x_null_skips_all_x_checks_under_either_policy(void **state)
{
  (void)state;
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };

  dt_remote_band_descriptor_t fixed = make_pure_fixed_descriptor();
  GArray *yv1 = make_values(y, G_N_ELEMENTS(y));
  dt_remote_error_t *err1 = NULL;
  assert_true(dt_remote_band_validate(&fixed, yv1, NULL, NULL, &err1));
  assert_null(err1);
  g_array_unref(yv1);

  dt_remote_band_descriptor_t interior = make_pure_interior_descriptor(0.001);
  GArray *yv2 = make_values(y, G_N_ELEMENTS(y));
  dt_remote_error_t *err2 = NULL;
  assert_true(dt_remote_band_validate(&interior, yv2, NULL, NULL, &err2));
  assert_null(err2);
  g_array_unref(yv2);
}

// Under FIXED, x is never checked by this pure function at all -- the
// field-level "x is not accepted under FIXED" rejection is
// dt_remote_band_apply_entries()'s job (DT_REMOTE_ERR_UNSUPPORTED_FIELD),
// not this value-level validator's. An obviously-invalid x (wrong length,
// unordered) is accepted here as long as y is valid, documenting that
// boundary explicitly.
static void test_band_validate_ignores_x_content_under_fixed_policy(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_fixed_descriptor();
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double bogus_x[] = { 0.9, 0.1 }; // wrong length AND unordered
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(bogus_x, G_N_ELEMENTS(bogus_x));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_band_validate(&desc, yv, xv, NULL, &err));
  assert_null(err);

  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_count_mismatch_under_interior(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.3, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", -1, "count_mismatch");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_first_endpoint_mismatch(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.05, 0.3, 0.6, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 0, "endpoint");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_last_endpoint_mismatch(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.3, 0.6, 0.95 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 3, "endpoint");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_without_stored_positions(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.3, 0.6, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, NULL, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 0, "endpoint");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_unordered(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.3, 0.2, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 2, "unordered");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_equal_adjacent(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.3, 0.3, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 2, "unordered");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_rejects_x_gap_below_minimum(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.0005, 0.6, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_band_error_details(err, "band.pure_interior", "x", 1, "gap");

  dt_remote_error_free(err);
  g_array_unref(yv);
  g_array_unref(xv);
}

static void test_band_validate_accepts_x_gap_exactly_at_minimum(void **state)
{
  (void)state;
  dt_remote_band_descriptor_t desc = make_pure_interior_descriptor(0.001);
  static const double y[] = { 0.1, 0.5, 0.9, 1.0 };
  static const double x[] = { 0.0, 0.001, 0.6, 1.0 };
  GArray *yv = make_values(y, G_N_ELEMENTS(y));
  GArray *xv = make_values(x, G_N_ELEMENTS(x));

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_band_validate(&desc, yv, xv, s_pure_stored_x, &err));
  assert_null(err);

  g_array_unref(yv);
  g_array_unref(xv);
}

/* ---------------------------------------------------------------------- */
/* harness: real "lowlight"/"atrous" module .so's via dt_init()            */
/* ---------------------------------------------------------------------- */

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_harness_confdir = NULL;
static GPtrArray *s_registry_test_allocations = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  s_registry_test_allocations = g_ptr_array_new_with_free_func(g_free);
  GError *gerror = NULL;
  s_harness_confdir = g_dir_make_tmp("test_remote_band-XXXXXX", &gerror);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_band: failed to create scratch config dir: %s\n", gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_band",
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
      fprintf(stderr, "test_remote_band: failed to remove scratch config dir %s\n", s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

static int lookup_override_test_setup(void **state)
{
  (void)state;
  dt_remote_band_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  dt_remote_vector_registry_set_lookup_override(NULL);
  return 0;
}

static int lookup_override_test_teardown(void **state)
{
  (void)state;
  dt_remote_band_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  dt_remote_vector_registry_set_lookup_override(NULL);
  return 0;
}

/* ---------------------------------------------------------------------- */
/* fixture: descriptor/adapter data, module loading                        */
/* ---------------------------------------------------------------------- */

typedef struct band_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} band_fixture_t;

// Generic real-module loader, same "adapter_fixture_new(op)" idiom
// test_remote_vector.c's real_vector_module_fixture_new() uses.
static band_fixture_t *real_band_module_fixture_new(const char *op)
{
  band_fixture_t *fixture = g_new0(band_fixture_t, 1);
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

static band_fixture_t *lowlight_fixture_new(void) { return real_band_module_fixture_new("lowlight"); }
static band_fixture_t *atrous_fixture_new(void) { return real_band_module_fixture_new("atrous"); }

static void band_fixture_free(band_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

// Writes `count` floats from `values` into the array `path` resolves to,
// against `module`'s live introspection tree and `params_blob` (which may
// be `module->params` or an unrelated scratch buffer of the same layout).
// Test-only direct-poke helper: bypasses the engine entirely so tests can
// establish deterministic native state (module default_params() values are
// not part of this engine's contract and should not be depended on).
static void poke_band_array(dt_iop_module_t *module, const dt_remote_introspection_path_t *path,
                            void *params_blob, const float *values, guint count)
{
  dt_introspection_t *intro = module->so->get_introspection();
  const dt_introspection_field_t *array_field = NULL;
  void *array_ptr = NULL;
  assert_true(dt_remote_path_resolve(path, intro->field, params_blob, &array_field, &array_ptr, NULL));
  for(guint i = 0; i < count; i++)
  {
    dt_introspection_field_t *element_field = NULL;
    void *element_ptr =
      dt_introspection_access_array((dt_introspection_field_t *)array_field, array_ptr, i, &element_field);
    assert_non_null(element_ptr);
    *(float *)element_ptr = values[i];
  }
}

static void read_band_array(dt_iop_module_t *module, const dt_remote_introspection_path_t *path,
                            const void *params_blob, float *out_values, guint count)
{
  dt_introspection_t *intro = module->so->get_introspection();
  const dt_introspection_field_t *array_field = NULL;
  void *array_ptr = NULL;
  assert_true(dt_remote_path_resolve(path, intro->field, (void *)params_blob, &array_field, &array_ptr,
                                     NULL));
  for(guint i = 0; i < count; i++)
  {
    dt_introspection_field_t *element_field = NULL;
    void *element_ptr =
      dt_introspection_access_array((dt_introspection_field_t *)array_field, array_ptr, i, &element_field);
    assert_non_null(element_ptr);
    out_values[i] = *(const float *)element_ptr;
  }
}

// "lowlight" native paths -- the generic fixture fields.
#define LOWLIGHT_BAND_COUNT 6
static const dt_remote_path_segment_t s_lowlight_transition_x_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "transition_x" },
};
static const dt_remote_introspection_path_t s_lowlight_transition_x_path = {
  .segments = s_lowlight_transition_x_segments, .length = G_N_ELEMENTS(s_lowlight_transition_x_segments)
};
static const dt_remote_path_segment_t s_lowlight_transition_y_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "transition_y" },
};
static const dt_remote_introspection_path_t s_lowlight_transition_y_path = {
  .segments = s_lowlight_transition_y_segments, .length = G_N_ELEMENTS(s_lowlight_transition_y_segments)
};
static const dt_remote_path_segment_t s_lowlight_blueness_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "blueness" },
};
static const dt_remote_introspection_path_t s_lowlight_blueness_path = {
  .segments = s_lowlight_blueness_segments, .length = G_N_ELEMENTS(s_lowlight_blueness_segments)
};
static const dt_remote_path_segment_t s_lowlight_missing_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "missing_band_field" },
};
static const dt_remote_introspection_path_t s_lowlight_missing_path = {
  .segments = s_lowlight_missing_segments, .length = G_N_ELEMENTS(s_lowlight_missing_segments)
};

static dt_remote_band_descriptor_t make_lowlight_descriptor(void)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.transition";
  desc.display_name = "Transition";
  desc.native_x = s_lowlight_transition_x_path;
  desc.native_y = s_lowlight_transition_y_path;
  desc.count = LOWLIGHT_BAND_COUNT;
  desc.y_minimum = 0.0;
  desc.y_maximum = 1.0;
  desc.x_policy = DT_REMOTE_BAND_X_FIXED;
  desc.minimum_gap = 0.0;
  desc.x_shared_with = NULL;
  return desc;
}

static dt_remote_band_module_adapter_t *new_lowlight_test_adapter(
  dt_remote_band_descriptor_t **out_descriptor)
{
  dt_remote_band_descriptor_t *descriptor = g_new(dt_remote_band_descriptor_t, 1);
  *descriptor = make_lowlight_descriptor();
  dt_remote_band_module_adapter_t *adapter = g_new0(dt_remote_band_module_adapter_t, 1);
  *adapter = (dt_remote_band_module_adapter_t){
    .operation = "lowlight",
    .minimum_params_version = 1,
    .maximum_params_version = 1,
    .bands = descriptor,
    .band_count = 1,
  };
  g_ptr_array_add(s_registry_test_allocations, descriptor);
  g_ptr_array_add(s_registry_test_allocations, adapter);
  *out_descriptor = descriptor;
  return adapter;
}

// "atrous" native paths (channel rows 0/1 of x[5][6]/y[5][6],
// atrous.c:77-78) -- the x-policy/twin fixture fields.
#define ATROUS_BAND_COUNT 6
static const dt_remote_path_segment_t s_atrous_x0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t s_atrous_x0_path = {
  .segments = s_atrous_x0_segments, .length = G_N_ELEMENTS(s_atrous_x0_segments)
};
static const dt_remote_path_segment_t s_atrous_y0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t s_atrous_y0_path = {
  .segments = s_atrous_y0_segments, .length = G_N_ELEMENTS(s_atrous_y0_segments)
};
static const dt_remote_path_segment_t s_atrous_x1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_introspection_path_t s_atrous_x1_path = {
  .segments = s_atrous_x1_segments, .length = G_N_ELEMENTS(s_atrous_x1_segments)
};
static const dt_remote_path_segment_t s_atrous_y1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_introspection_path_t s_atrous_y1_path = {
  .segments = s_atrous_y1_segments, .length = G_N_ELEMENTS(s_atrous_y1_segments)
};

static dt_remote_band_descriptor_t make_atrous_twin_a(void)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.twin_a";
  desc.display_name = "Twin A";
  desc.native_x = s_atrous_x0_path;
  desc.native_y = s_atrous_y0_path;
  desc.count = ATROUS_BAND_COUNT;
  desc.y_minimum = 0.0;
  desc.y_maximum = 1.0;
  desc.x_policy = DT_REMOTE_BAND_X_INTERIOR;
  desc.minimum_gap = 0.001;
  desc.x_shared_with = "band.twin_b";
  return desc;
}

static dt_remote_band_descriptor_t make_atrous_twin_b(void)
{
  dt_remote_band_descriptor_t desc = { 0 };
  desc.name = "band.twin_b";
  desc.display_name = "Twin B";
  desc.native_x = s_atrous_x1_path;
  desc.native_y = s_atrous_y1_path;
  desc.count = ATROUS_BAND_COUNT;
  desc.y_minimum = 0.0;
  desc.y_maximum = 1.0;
  desc.x_policy = DT_REMOTE_BAND_X_INTERIOR;
  desc.minimum_gap = 0.001;
  desc.x_shared_with = "band.twin_a";
  return desc;
}

static dt_remote_band_module_adapter_t *new_atrous_twin_adapter(
  dt_remote_band_descriptor_t **out_descriptors /* [2] */)
{
  dt_remote_band_descriptor_t *descriptors = g_new(dt_remote_band_descriptor_t, 2);
  descriptors[0] = make_atrous_twin_a();
  descriptors[1] = make_atrous_twin_b();
  dt_remote_band_module_adapter_t *adapter = g_new0(dt_remote_band_module_adapter_t, 1);
  *adapter = (dt_remote_band_module_adapter_t){
    .operation = "atrous",
    .minimum_params_version = 2,
    .maximum_params_version = 2,
    .bands = descriptors,
    .band_count = 2,
  };
  g_ptr_array_add(s_registry_test_allocations, descriptors);
  g_ptr_array_add(s_registry_test_allocations, adapter);
  if(out_descriptors)
  {
    out_descriptors[0] = &descriptors[0];
    out_descriptors[1] = &descriptors[1];
  }
  return adapter;
}

static void assert_band_registry_rejects(const dt_remote_band_module_adapter_t *adapter,
                                         const dt_iop_module_so_t *so)
{
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_registry_validate(adapter, so, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

static const dt_remote_band_module_adapter_t *s_lookup_override_adapter = NULL;

static const dt_remote_band_module_adapter_t *private_band_lookup_override(const char *operation,
                                                                            guint params_version)
{
  if(!s_lookup_override_adapter || g_strcmp0(operation, s_lookup_override_adapter->operation)
     || params_version < s_lookup_override_adapter->minimum_params_version
     || params_version > s_lookup_override_adapter->maximum_params_version)
    return NULL;
  return s_lookup_override_adapter;
}

static void install_band_adapter(const dt_remote_band_module_adapter_t *adapter)
{
  s_lookup_override_adapter = adapter;
  dt_remote_band_registry_set_lookup_override(private_band_lookup_override);
}

// Synthetic curve/vector adapters installed only for the cross-class
// collision tests below; their descriptors are never resolved against real
// introspection (dt_remote_band_registry_validate() only reads their
// `.name`), so minimal, unvalidated descriptors are sufficient.
static const dt_remote_curve_descriptor_t s_colliding_curve_descriptor = {
  .name = "band.transition", // deliberately identical to make_lowlight_descriptor()'s name
};
static const dt_remote_curve_module_adapter_t s_colliding_curve_adapter = {
  .operation = "lowlight",
  .minimum_params_version = 1,
  .maximum_params_version = 1,
  .curves = &s_colliding_curve_descriptor,
  .curve_count = 1,
};
static const dt_remote_curve_module_adapter_t *colliding_curve_lookup_override(const char *operation,
                                                                                guint params_version)
{
  if(g_strcmp0(operation, "lowlight") || params_version != 1) return NULL;
  return &s_colliding_curve_adapter;
}

static const dt_remote_vector_component_t s_colliding_vector_components[1] = { { "v", 0.0, 1.0 } };
static const dt_remote_vector_descriptor_t s_colliding_vector_descriptor = {
  .name = "band.transition", // deliberately identical to make_lowlight_descriptor()'s name
  .component_count = 1,
  .components = s_colliding_vector_components,
};
static const dt_remote_vector_module_adapter_t s_colliding_vector_adapter = {
  .operation = "lowlight",
  .minimum_params_version = 1,
  .maximum_params_version = 1,
  .vectors = &s_colliding_vector_descriptor,
  .vector_count = 1,
};
static const dt_remote_vector_module_adapter_t *colliding_vector_lookup_override(const char *operation,
                                                                                  guint params_version)
{
  if(g_strcmp0(operation, "lowlight") || params_version != 1) return NULL;
  return &s_colliding_vector_adapter;
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_registry_validate                                        */
/* ---------------------------------------------------------------------- */

static void test_registry_validate_rejects_invalid_adapter_envelope(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;

  dt_remote_band_module_adapter_t *wrong_operation = new_lowlight_test_adapter(&descriptor);
  wrong_operation->operation = "atrous";
  assert_band_registry_rejects(wrong_operation, so);

  dt_remote_band_module_adapter_t *reversed_versions = new_lowlight_test_adapter(&descriptor);
  reversed_versions->minimum_params_version = 2;
  reversed_versions->maximum_params_version = 1;
  assert_band_registry_rejects(reversed_versions, so);

  dt_remote_band_module_adapter_t *outside_version = new_lowlight_test_adapter(&descriptor);
  outside_version->minimum_params_version = 5;
  outside_version->maximum_params_version = 9;
  assert_band_registry_rejects(outside_version, so);

  dt_remote_band_module_adapter_t *null_bands = new_lowlight_test_adapter(&descriptor);
  null_bands->bands = NULL;
  assert_band_registry_rejects(null_bands, so);
}

static void test_registry_validate_rejects_native_x_missing(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  descriptor->native_x = s_lowlight_missing_path;
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_native_y_missing(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  descriptor->native_y = s_lowlight_missing_path;
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_native_x_wrong_type(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  descriptor->native_x = s_lowlight_blueness_path; // a scalar float, not an array
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_native_array_length_mismatch(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  descriptor->count = 5; // real transition_x/transition_y are length 6
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_duplicate_names(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);

  dt_remote_band_descriptor_t *descriptors = g_new(dt_remote_band_descriptor_t, 2);
  descriptors[0] = make_lowlight_descriptor();
  descriptors[1] = make_lowlight_descriptor(); // same name, "band.transition"
  dt_remote_band_module_adapter_t *adapter = g_new0(dt_remote_band_module_adapter_t, 1);
  *adapter = (dt_remote_band_module_adapter_t){
    .operation = "lowlight", .minimum_params_version = 1, .maximum_params_version = 1,
    .bands = descriptors, .band_count = 2,
  };
  g_ptr_array_add(s_registry_test_allocations, descriptors);
  g_ptr_array_add(s_registry_test_allocations, adapter);

  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_collision_with_curve(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);

  dt_remote_curve_registry_set_lookup_override(colliding_curve_lookup_override);
  assert_band_registry_rejects(adapter, so);
  dt_remote_curve_registry_set_lookup_override(NULL);
}

static void test_registry_validate_rejects_collision_with_vector(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);

  dt_remote_vector_registry_set_lookup_override(colliding_vector_lookup_override);
  assert_band_registry_rejects(adapter, so);
  dt_remote_vector_registry_set_lookup_override(NULL);
}

static void test_registry_validate_rejects_x_shared_with_missing_twin(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  dt_remote_band_module_adapter_t *adapter = new_atrous_twin_adapter(descs);
  descs[0]->x_shared_with = "band.nonexistent";
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_x_shared_with_count_mismatch(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  dt_remote_band_module_adapter_t *adapter = new_atrous_twin_adapter(descs);
  // Also breaks descs[1]'s own native-array-length check (the real x[1]/y[1]
  // arrays stay length 6) -- fail-closed either way is the point of this
  // test; both the native-shape check and the twin-symmetry check would
  // independently reject this adapter.
  descs[1]->count = 5;
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_x_shared_with_asymmetric(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  dt_remote_band_module_adapter_t *adapter = new_atrous_twin_adapter(descs);
  descs[1]->x_shared_with = "band.not_twin_a"; // does not point back at descs[0]
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_interior_minimum_gap_not_positive(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  assert_non_null(so);

  const double bad_gaps[] = { 0.0, -0.001, -FLT_EPSILON };
  for(guint i = 0; i < G_N_ELEMENTS(bad_gaps); i++)
  {
    dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
    dt_remote_band_module_adapter_t *adapter = new_atrous_twin_adapter(descs);
    descs[0]->minimum_gap = bad_gaps[i];
    assert_band_registry_rejects(adapter, so);
  }
}

static void test_registry_validate_rejects_fixed_with_nonzero_minimum_gap(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  descriptor->minimum_gap = 0.001; // FIXED requires exactly 0.0
  assert_band_registry_rejects(adapter, so);
}

static void test_registry_validate_accepts_valid_fixed_adapter(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_registry_validate(adapter, so, &error));
  assert_null(error);
}

static void test_registry_validate_accepts_valid_interior_twin_adapter(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  assert_non_null(so);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  dt_remote_band_module_adapter_t *adapter = new_atrous_twin_adapter(descs);

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_registry_validate(adapter, so, &error));
  assert_null(error);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_list_schema                                              */
/* ---------------------------------------------------------------------- */

static void test_list_schema_converts_fixed_descriptor(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));

  GPtrArray *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_list_schema(so, &out, &error));
  assert_null(error);
  assert_non_null(out);
  assert_int_equal(out->len, 1);

  const dt_remote_band_schema_t *schema = g_ptr_array_index(out, 0);
  assert_string_equal(schema->name, "band.transition");
  assert_int_equal(schema->count, LOWLIGHT_BAND_COUNT);
  assert_float_equal(schema->y_minimum, 0.0, 1e-9);
  assert_float_equal(schema->y_maximum, 1.0, 1e-9);
  assert_int_equal(schema->x_policy, DT_REMOTE_BAND_X_FIXED);
  assert_float_equal(schema->minimum_gap, 0.0, 1e-9);
  assert_null(schema->x_shared_with);
  assert_null(schema->x); // no live params blob at the list_schema level
  assert_int_equal(schema->writability, DT_REMOTE_WRITABLE_NOW);
  assert_null(schema->active_when);
  assert_null(schema->writable_when);

  g_ptr_array_unref(out);
}

static void test_list_schema_converts_interior_twin_pair_in_registry_order(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("atrous");
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));

  GPtrArray *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_list_schema(so, &out, &error));
  assert_null(error);
  assert_non_null(out);
  assert_int_equal(out->len, 2);

  const dt_remote_band_schema_t *a = g_ptr_array_index(out, 0);
  const dt_remote_band_schema_t *b = g_ptr_array_index(out, 1);
  assert_string_equal(a->name, "band.twin_a");
  assert_string_equal(b->name, "band.twin_b");
  assert_int_equal(a->x_policy, DT_REMOTE_BAND_X_INTERIOR);
  assert_float_equal(a->minimum_gap, 0.001, 1e-9);
  assert_string_equal(a->x_shared_with, "band.twin_b");
  assert_string_equal(b->x_shared_with, "band.twin_a");

  g_ptr_array_unref(out);
}

static void test_list_schema_no_adapter_yields_null_and_succeeds(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("lowlight");

  GPtrArray *out = (GPtrArray *)0x1; // poison, must be reset to NULL
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_list_schema(so, &out, &error));
  assert_null(error);
  assert_null(out);
}

static void test_list_schema_null_arguments_fail(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  GPtrArray *out = NULL;
  assert_false(dt_remote_band_list_schema(NULL, &out, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(out);
  dt_remote_error_free(error);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_read_values                                              */
/* ---------------------------------------------------------------------- */

static void test_read_values_widens_floats_and_stamps_unconditional_active(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));

  static const float known_x[LOWLIGHT_BAND_COUNT] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
  static const float known_y[LOWLIGHT_BAND_COUNT] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f };
  poke_band_array(fixture->module, &s_lowlight_transition_x_path, fixture->module->params, known_x,
                  LOWLIGHT_BAND_COUNT);
  poke_band_array(fixture->module, &s_lowlight_transition_y_path, fixture->module->params, known_y,
                  LOWLIGHT_BAND_COUNT);

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_null(error);
  assert_non_null(out);

  const dt_remote_band_value_t *value = g_hash_table_lookup(out, "band.transition");
  assert_non_null(value);
  assert_int_equal(value->y->len, LOWLIGHT_BAND_COUNT);
  assert_int_equal(value->x->len, LOWLIGHT_BAND_COUNT);
  for(guint i = 0; i < LOWLIGHT_BAND_COUNT; i++)
  {
    assert_float_equal(g_array_index(value->y, double, i), (double)known_y[i], 1e-6);
    assert_float_equal(g_array_index(value->x, double, i), (double)known_x[i], 1e-6);
  }
  assert_true(value->active);
  assert_true(value->effective);
  assert_true(value->writable_now);

  g_hash_table_unref(out);
  band_fixture_free(fixture);
}

static void test_read_values_no_adapter_returns_empty_table(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_null(error);
  assert_non_null(out);
  assert_int_equal(g_hash_table_size(out), 0);

  g_hash_table_unref(out);
  band_fixture_free(fixture);
}

static void test_read_values_null_arguments_fail(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  GHashTable *out = NULL;
  assert_false(dt_remote_band_read_values(NULL, NULL, &out, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(out);
  dt_remote_error_free(error);
}

// The registry-validate predicate check (band_predicate_is_valid(),
// remote_band_registry.c) rejects a predicate field that does not resolve
// to a live enum before read_values ever reaches its own runtime
// predicate_holds() call -- this proves the failure propagates cleanly
// through dt_remote_band_read_values() end to end (fail closed, table
// freed, *out NULL). Neither fixture module has a real enum field to
// exercise the EQ/NE *matching* branches themselves; that logic is shared,
// unmodified code already proven exhaustively against "borders"'s
// aspect_orient enum in test_remote_vector.c.
static const dt_remote_parameter_predicate_t s_unresolvable_predicate = {
  .field = "missing_enum_field", .op = DT_REMOTE_PREDICATE_EQ, .enum_name = "WHATEVER"
};

static void test_read_values_predicate_drift_fails_closed(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  descriptor->active_when = &s_unresolvable_predicate;

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(out);

  dt_remote_error_free(error);
  band_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_apply_entries / dt_remote_band_apply_patch               */
/* ---------------------------------------------------------------------- */

static dt_remote_semantic_patch_t *make_band_entry(const char *name, const double *y, guint yn,
                                                    const double *x, guint xn)
{
  dt_remote_semantic_patch_t *entry = g_new0(dt_remote_semantic_patch_t, 1);
  entry->class_id = DT_REMOTE_PARAMETER_BANDS;
  entry->value.bands.name = g_strdup(name);
  entry->value.bands.y = make_values(y, yn);
  entry->value.bands.x = x ? make_values(x, xn) : NULL;
  return entry;
}

static void *scratch_params_new(dt_iop_module_t *module)
{
  void *scratch = g_malloc(module->params_size);
  memcpy(scratch, module->params, module->params_size);
  return scratch;
}

static void test_apply_entries_unknown_id_with_adapter(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  install_band_adapter(adapter);
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  dt_remote_semantic_patch_t *entry = make_band_entry("band.nonexistent", y, LOWLIGHT_BAND_COUNT, NULL, 0);
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, entry);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_patch_unknown_id_no_adapter_at_all(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  // deliberately install nothing: production registry is empty
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(patch.semantic_values,
                  make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                          &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);

  dt_remote_error_free(error);
  g_ptr_array_unref(patch.semantic_values);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_y_length_mismatch(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[5] = { 0.1, 0.2, 0.3, 0.4, 0.5 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.transition", y, 5, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_x_under_fixed_policy_rejected(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  static const double x[LOWLIGHT_BAND_COUNT] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries,
                  make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, x, LOWLIGHT_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNSUPPORTED_FIELD);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_y_domain_violation_leaves_scratch_untouched(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 1.5 }; // last one out of range
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

static void test_apply_entries_successful_write_preserves_other_bytes(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);
  void *expected = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.05, 0.15, 0.25, 0.35, 0.45, 0.55 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                           &error));
  assert_null(error);

  // Build the expected buffer by poking only transition_y into a copy of
  // the pre-call snapshot, exactly the narrowing the production write path
  // performs -- then a full params_size memcmp proves every other byte
  // (transition_x, blueness, and any padding) is untouched.
  float narrowed_y[LOWLIGHT_BAND_COUNT];
  for(guint i = 0; i < LOWLIGHT_BAND_COUNT; i++) narrowed_y[i] = (float)y[i];
  poke_band_array(fixture->module, &s_lowlight_transition_y_path, expected, narrowed_y,
                  LOWLIGHT_BAND_COUNT);
  assert_memory_equal(scratch, expected, fixture->module->params_size);

  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(expected);
  band_fixture_free(fixture);
}

static void test_apply_entries_duplicate_id_rejected(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));
  g_ptr_array_add(entries, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static int s_validate_completed_calls = 0;
static gboolean s_validate_completed_result = TRUE;
static float s_validate_completed_expected_y0 = 0.0f;

static gboolean test_validate_completed(const struct dt_remote_band_context_t *ctx,
                                        const void *new_params, dt_remote_error_t **error)
{
  s_validate_completed_calls++;
  // Observes the already-narrowed write in `new_params`, proving
  // validate_completed() runs after every per-descriptor write in this
  // call -- same ordering property test_remote_vector.c's
  // validate_completed_observes_both_vectors() proves for the vector twin.
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!dt_remote_path_resolve(&s_lowlight_transition_y_path, ctx->introspection->field, (void *)new_params,
                             &field, &ptr, error))
    return FALSE;
  dt_introspection_field_t *element_field = NULL;
  const float *value =
    dt_introspection_access_array((dt_introspection_field_t *)field, ptr, 0, &element_field);
  if(!value || *value != s_validate_completed_expected_y0)
  {
    if(error)
    {
      *error = g_new0(dt_remote_error_t, 1);
      (*error)->code = DT_REMOTE_ERR_INVALID_VALUE;
      (*error)->message = g_strdup("validate_completed ran before the band write");
    }
    return FALSE;
  }

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

static void test_apply_entries_validate_completed_observes_write_and_can_reject(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  adapter->validate_completed = test_validate_completed;
  install_band_adapter(adapter);
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.07, 0.2, 0.3, 0.4, 0.5, 0.6 };
  s_validate_completed_expected_y0 = (float)y[0];
  s_validate_completed_calls = 0;
  s_validate_completed_result = TRUE;

  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                           &error));
  assert_null(error);
  assert_int_equal(s_validate_completed_calls, 1);

  // Now make validate_completed reject: the whole call fails and returns
  // its error, even though the per-descriptor write already landed in
  // `scratch` -- rollback on rejection is the caller's job (a discarded
  // temp blob), not this function's, per dt_remote_band_apply_entries()'s
  // own doc comment.
  g_free(scratch);
  scratch = scratch_params_new(fixture->module);
  s_validate_completed_calls = 0;
  s_validate_completed_result = FALSE;
  GPtrArray *entries2 = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries2, make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));
  dt_remote_error_t *error2 = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries2,
                                            &error2));
  assert_non_null(error2);
  assert_int_equal(error2->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_int_equal(s_validate_completed_calls, 1);

  dt_remote_error_free(error2);
  g_ptr_array_unref(entries);
  g_ptr_array_unref(entries2);
  g_free(scratch);
  band_fixture_free(fixture);
}

static dt_remote_patch_entry_t *make_scalar_entry(const char *name)
{
  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup(name);
  entry->value.type = DT_REMOTE_VALUE_FLOAT;
  entry->value.v.f = 0.0;
  return entry;
}

static const char *const s_lowlight_prepare_fields[] = { "blueness" };

static void test_apply_patch_prepare_field_gate_triggers_with_zero_band_entries(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  adapter->prepare_fields = s_lowlight_prepare_fields;
  adapter->prepare_field_count = 1;
  adapter->validate_completed = test_validate_completed;
  install_band_adapter(adapter);
  void *scratch = scratch_params_new(fixture->module);

  float current_y[LOWLIGHT_BAND_COUNT];
  read_band_array(fixture->module, &s_lowlight_transition_y_path, scratch, current_y, LOWLIGHT_BAND_COUNT);
  s_validate_completed_expected_y0 = current_y[0];
  s_validate_completed_calls = 0;
  s_validate_completed_result = TRUE;

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values, make_scalar_entry("blueness"));
  patch.semantic_values = NULL;

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                         &error));
  assert_null(error);
  assert_int_equal(s_validate_completed_calls, 1);

  g_ptr_array_unref(patch.scalar_values);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_patch_no_relevant_content_returns_true_without_touching_registry(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  dt_remote_band_module_adapter_t *adapter = new_lowlight_test_adapter(&descriptor);
  // Deliberately broken (would fail registry_validate() if it ever ran):
  adapter->minimum_params_version = 9;
  adapter->maximum_params_version = 1;
  install_band_adapter(adapter);
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  dt_remote_patch_t patch = { 0 };

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                         &error));
  assert_null(error);
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

static const dt_remote_curve_patch_t s_dummy_curve_patch = { 0 };

static void test_apply_patch_mixed_class_entries_skip_non_bands(void **state)
{
  (void)state;
  band_fixture_t *fixture = lowlight_fixture_new();
  dt_remote_band_descriptor_t *descriptor = NULL;
  install_band_adapter(new_lowlight_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[LOWLIGHT_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);

  dt_remote_semantic_patch_t *curve_entry = g_new0(dt_remote_semantic_patch_t, 1);
  curve_entry->class_id = DT_REMOTE_PARAMETER_CURVE;
  curve_entry->value.curve = s_dummy_curve_patch;
  curve_entry->value.curve.name = g_strdup("dummy.curve");
  g_ptr_array_add(patch.semantic_values, curve_entry);
  g_ptr_array_add(patch.semantic_values,
                  make_band_entry("band.transition", y, LOWLIGHT_BAND_COUNT, NULL, 0));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                         &error));
  assert_null(error);

  float written_y[LOWLIGHT_BAND_COUNT];
  read_band_array(fixture->module, &s_lowlight_transition_y_path, scratch, written_y, LOWLIGHT_BAND_COUNT);
  for(guint i = 0; i < LOWLIGHT_BAND_COUNT; i++)
    assert_float_equal(written_y[i], (float)y[i], 1e-6);

  g_ptr_array_unref(patch.semantic_values);
  g_free(scratch);
  band_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_band_apply_entries -- INTERIOR x policy / twin sharing        */
/* (atrous fixture)                                                        */
/* ---------------------------------------------------------------------- */

static const float s_atrous_stored_x[ATROUS_BAND_COUNT] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
static const float s_atrous_stored_y[ATROUS_BAND_COUNT] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };

// Establishes deterministic stored x/y for both twin channels (0 and 1) --
// module default_params() values are not part of this engine's contract.
static void seed_atrous_channels(band_fixture_t *fixture)
{
  poke_band_array(fixture->module, &s_atrous_x0_path, fixture->module->params, s_atrous_stored_x,
                  ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_y0_path, fixture->module->params, s_atrous_stored_y,
                  ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_x1_path, fixture->module->params, s_atrous_stored_x,
                  ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_y1_path, fixture->module->params, s_atrous_stored_y,
                  ATROUS_BAND_COUNT);
}

// Like seed_atrous_channels() but with genuinely different stored x
// endpoints per channel -- needed to prove the endpoint check for the
// SECOND-processed twin in a registry-order write loop is checked against
// its own true pre-transaction state, not against whatever the
// FIRST-processed twin's mirror write may already have placed in its
// native_x within the same call.
static void seed_atrous_channels_divergent(band_fixture_t *fixture, const float *x_a, const float *x_b)
{
  poke_band_array(fixture->module, &s_atrous_x0_path, fixture->module->params, x_a, ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_y0_path, fixture->module->params, s_atrous_stored_y,
                  ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_x1_path, fixture->module->params, x_b, ATROUS_BAND_COUNT);
  poke_band_array(fixture->module, &s_atrous_y1_path, fixture->module->params, s_atrous_stored_y,
                  ATROUS_BAND_COUNT);
}

// Regression test for the review finding: without checking endpoint
// pinning against `old_params`, twin_b's own endpoint check would read its
// "stored" x from `new_params` -- which, by the time twin_b's turn comes
// up in the registry-order write loop, already contains the value twin_a's
// earlier write just mirrored into it -- making the check compare the
// submission against itself (always passing) instead of against twin_b's
// true pre-transaction endpoints.
static void test_apply_entries_twin_endpoint_check_uses_pre_transaction_state(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  static const float stored_x_a[ATROUS_BAND_COUNT] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
  static const float stored_x_b[ATROUS_BAND_COUNT] = { 0.05f, 0.2f, 0.4f, 0.6f, 0.8f, 0.95f };
  seed_atrous_channels_divergent(fixture, stored_x_a, stored_x_b);

  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  // Matches twin_a's own stored endpoints (0.0/1.0, processed first in
  // registry order) but NOT twin_b's genuinely different stored endpoints
  // (0.05/0.95): twin_b's own endpoint check must still reject this on its
  // own turn.
  static const double x[ATROUS_BAND_COUNT] = { 0.0, 0.21, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));
  g_ptr_array_add(entries, make_band_entry("band.twin_b", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  // The two-pass validate-then-write structure (validate_band_entry() for
  // every entry before write_band_patch() for any of them) means no write
  // happens at all when any entry's value-level validation fails --
  // stronger than the general "earlier entries may already be written"
  // caveat, which only still applies to the predicate-gating failure mode.
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

// Single-entry counterpart: only twin_a is patched, and twin_b's stored
// endpoints genuinely diverge from twin_a's. Design decision (documented
// in the task report): twin_b's endpoint is never independently checked in
// this case -- the mirror overwrites twin_b's entire x array
// unconditionally, exactly matching atrous.c's own "an x drag mirrors into
// the twin" GUI behavior (milestone 5 design doc). This is intentional,
// not a residual instance of the bug fixed above: that bug was about a
// twin's own *validated* entry becoming vacuously checked, not about the
// always-unconditional mirror onto a twin with no entry of its own in this
// call.
static void test_apply_entries_twin_sync_overwrites_divergent_twin_endpoints(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  static const float stored_x_a[ATROUS_BAND_COUNT] = { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f };
  static const float stored_x_b[ATROUS_BAND_COUNT] = { 0.05f, 0.2f, 0.4f, 0.6f, 0.8f, 0.95f };
  seed_atrous_channels_divergent(fixture, stored_x_a, stored_x_b);

  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  // Matches only twin_a's own stored endpoints; twin_b has no entry at all
  // in this request.
  static const double x[ATROUS_BAND_COUNT] = { 0.0, 0.25, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                           &error));
  assert_null(error);

  float twin_b_x[ATROUS_BAND_COUNT];
  read_band_array(fixture->module, &s_atrous_x1_path, scratch, twin_b_x, ATROUS_BAND_COUNT);
  for(guint i = 0; i < ATROUS_BAND_COUNT; i++)
    assert_float_equal(twin_b_x[i], (float)x[i], 1e-6);

  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_twin_conflict_differing_x_rejected(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  seed_atrous_channels(fixture);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  static const double x_a[ATROUS_BAND_COUNT] = { 0.0, 0.21, 0.4, 0.6, 0.8, 1.0 };
  static const double x_b[ATROUS_BAND_COUNT] = { 0.0, 0.22, 0.4, 0.6, 0.8, 1.0 }; // differs at index 1
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x_a, ATROUS_BAND_COUNT));
  g_ptr_array_add(entries, make_band_entry("band.twin_b", y, ATROUS_BAND_COUNT, x_b, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

static void test_apply_entries_twin_sync_propagates_x_without_twin_entry(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  seed_atrous_channels(fixture);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  static const double x[ATROUS_BAND_COUNT] = { 0.0, 0.25, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                           &error));
  assert_null(error);

  float twin_a_x[ATROUS_BAND_COUNT], twin_b_x[ATROUS_BAND_COUNT];
  float twin_a_y[ATROUS_BAND_COUNT], twin_b_y[ATROUS_BAND_COUNT];
  read_band_array(fixture->module, &s_atrous_x0_path, scratch, twin_a_x, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_x1_path, scratch, twin_b_x, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_y0_path, scratch, twin_a_y, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_y1_path, scratch, twin_b_y, ATROUS_BAND_COUNT);

  for(guint i = 0; i < ATROUS_BAND_COUNT; i++)
  {
    assert_float_equal(twin_a_x[i], (float)x[i], 1e-6);
    // Twin B's x is synced even though twin B had no entry of its own.
    assert_float_equal(twin_b_x[i], (float)x[i], 1e-6);
    assert_float_equal(twin_a_y[i], (float)y[i], 1e-6);
    // Twin B's y is untouched: only x sharing is mirrored, never y.
    assert_float_equal(twin_b_y[i], s_atrous_stored_y[i], 1e-6);
  }

  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_twin_matching_x_both_entries_succeeds(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  seed_atrous_channels(fixture);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);

  static const double y_a[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  static const double y_b[ATROUS_BAND_COUNT] = { 0.6, 0.5, 0.4, 0.3, 0.2, 0.1 };
  static const double x[ATROUS_BAND_COUNT] = { 0.0, 0.25, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y_a, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));
  g_ptr_array_add(entries, make_band_entry("band.twin_b", y_b, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                           &error));
  assert_null(error);

  float twin_a_x[ATROUS_BAND_COUNT], twin_b_x[ATROUS_BAND_COUNT];
  float twin_a_y[ATROUS_BAND_COUNT], twin_b_y[ATROUS_BAND_COUNT];
  read_band_array(fixture->module, &s_atrous_x0_path, scratch, twin_a_x, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_x1_path, scratch, twin_b_x, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_y0_path, scratch, twin_a_y, ATROUS_BAND_COUNT);
  read_band_array(fixture->module, &s_atrous_y1_path, scratch, twin_b_y, ATROUS_BAND_COUNT);
  for(guint i = 0; i < ATROUS_BAND_COUNT; i++)
  {
    assert_float_equal(twin_a_x[i], (float)x[i], 1e-6);
    assert_float_equal(twin_b_x[i], (float)x[i], 1e-6);
    assert_float_equal(twin_a_y[i], (float)y_a[i], 1e-6);
    assert_float_equal(twin_b_y[i], (float)y_b[i], 1e-6);
  }

  g_ptr_array_unref(entries);
  g_free(scratch);
  band_fixture_free(fixture);
}

static void test_apply_entries_endpoint_mismatch_rejected(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  seed_atrous_channels(fixture);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  // index 0 does not match the stored 0.0f endpoint.
  static const double x[ATROUS_BAND_COUNT] = { 0.05, 0.25, 0.4, 0.6, 0.8, 1.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

static void test_apply_entries_gap_below_minimum_rejected(void **state)
{
  (void)state;
  band_fixture_t *fixture = atrous_fixture_new();
  seed_atrous_channels(fixture);
  dt_remote_band_descriptor_t *descs[2] = { NULL, NULL };
  install_band_adapter(new_atrous_twin_adapter(descs));
  void *scratch = scratch_params_new(fixture->module);
  void *snapshot = scratch_params_new(fixture->module);

  static const double y[ATROUS_BAND_COUNT] = { 0.1, 0.2, 0.3, 0.4, 0.5, 0.6 };
  static const double x[ATROUS_BAND_COUNT] = { 0.0, 0.0005, 0.4, 0.6, 0.8, 1.0 }; // gap 0.0005 < 0.001
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_band_entry("band.twin_a", y, ATROUS_BAND_COUNT, x, ATROUS_BAND_COUNT));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(fixture->module, fixture->module->params, scratch, entries,
                                            &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(scratch, snapshot, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(snapshot);
  band_fixture_free(fixture);
}

static void test_apply_entries_null_arguments_fail(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_entries(NULL, NULL, NULL, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

static void test_apply_patch_null_arguments_fail(void **state)
{
  (void)state;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_band_apply_patch(NULL, NULL, NULL, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

/* ---------------------------------------------------------------------- */
/* main                                                                     */
/* ---------------------------------------------------------------------- */

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_band_schema_free_fully_populated),
    cmocka_unit_test(test_band_schema_free_partially_populated),
    cmocka_unit_test(test_band_schema_free_null_is_safe),
    cmocka_unit_test(test_band_value_free_fully_populated),
    cmocka_unit_test(test_band_value_free_partially_populated),
    cmocka_unit_test(test_band_value_free_null_is_safe),

    cmocka_unit_test(test_band_validate_accepts_well_formed_y_only),
    cmocka_unit_test(test_band_validate_rejects_y_count_mismatch),
    cmocka_unit_test(test_band_validate_rejects_y_non_finite),
    cmocka_unit_test(test_band_validate_rejects_y_below_minimum),
    cmocka_unit_test(test_band_validate_rejects_y_above_maximum_at_double_boundary),
    cmocka_unit_test(test_band_validate_accepts_y_exactly_at_maximum),
    cmocka_unit_test(test_band_validate_rejects_y_exceeding_native_float_range),
    cmocka_unit_test(test_band_validate_accepts_y_exactly_at_native_float_range_boundary),
    cmocka_unit_test(test_band_validate_never_mutates_y_on_rejection),
    cmocka_unit_test(test_band_validate_x_null_skips_all_x_checks_under_either_policy),
    cmocka_unit_test(test_band_validate_ignores_x_content_under_fixed_policy),
    cmocka_unit_test(test_band_validate_rejects_x_count_mismatch_under_interior),
    cmocka_unit_test(test_band_validate_rejects_x_first_endpoint_mismatch),
    cmocka_unit_test(test_band_validate_rejects_x_last_endpoint_mismatch),
    cmocka_unit_test(test_band_validate_rejects_x_without_stored_positions),
    cmocka_unit_test(test_band_validate_rejects_x_unordered),
    cmocka_unit_test(test_band_validate_rejects_x_equal_adjacent),
    cmocka_unit_test(test_band_validate_rejects_x_gap_below_minimum),
    cmocka_unit_test(test_band_validate_accepts_x_gap_exactly_at_minimum),

    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_invalid_adapter_envelope,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_native_x_missing,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_native_y_missing,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_native_x_wrong_type,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_native_array_length_mismatch,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_duplicate_names,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_collision_with_curve,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_collision_with_vector,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_x_shared_with_missing_twin,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_x_shared_with_count_mismatch,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_x_shared_with_asymmetric,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_interior_minimum_gap_not_positive,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_fixed_with_nonzero_minimum_gap,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_accepts_valid_fixed_adapter,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_accepts_valid_interior_twin_adapter,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_list_schema_converts_fixed_descriptor,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_converts_interior_twin_pair_in_registry_order,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_no_adapter_yields_null_and_succeeds,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_read_values_widens_floats_and_stamps_unconditional_active,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_no_adapter_returns_empty_table,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_predicate_drift_fails_closed,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_apply_entries_unknown_id_with_adapter,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_unknown_id_no_adapter_at_all,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_y_length_mismatch,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_x_under_fixed_policy_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_y_domain_violation_leaves_scratch_untouched,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_successful_write_preserves_other_bytes,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_duplicate_id_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_validate_completed_observes_write_and_can_reject,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_prepare_field_gate_triggers_with_zero_band_entries,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(
      test_apply_patch_no_relevant_content_returns_true_without_touching_registry,
      lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_mixed_class_entries_skip_non_bands,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_apply_entries_twin_endpoint_check_uses_pre_transaction_state,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_twin_sync_overwrites_divergent_twin_endpoints,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_twin_conflict_differing_x_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_twin_sync_propagates_x_without_twin_entry,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_twin_matching_x_both_entries_succeeds,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_endpoint_mismatch_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_gap_below_minimum_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
