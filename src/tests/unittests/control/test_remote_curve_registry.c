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
 * cmocka unit tests for the curve module registry, the rgbcurve semantic
 * descriptor table, and the read-only half of the curve engine API
 * (src/control/remote_curve_registry.c):
 *
 *  - dt_remote_curve_registry_lookup(): finds the rgbcurve adapter by its
 *    stable operation name and params_version, and returns NULL for an
 *    unknown op or an unsupported version.
 *  - dt_remote_curve_registry_validate(): passes against the real loaded
 *    rgbcurve module's introspection tree, rejects (DT_REMOTE_ERR_INTERNAL)
 *    a descriptor whose native node array is the wrong element type, and
 *    caches its result per adapter identity (a second call with a
 *    *different* introspection tree for the same adapter still returns the
 *    first, cached result -- the shape check is not re-run every call).
 *    The shape-mismatch/caching test uses a small hand-built private
 *    descriptor/adapter/introspection fixture, entirely independent of the
 *    real rgbcurve adapter, so it cannot collide with the other tests'
 *    shared use of the process-lifetime cache for the *real* adapter.
 *  - the exact stable enum name the introspection generator assigns to
 *    DT_S_SCALE_MANUAL_RGB is confirmed against the real loaded module
 *    (tools/introspection/ast.pm emits the C enumerator token verbatim as
 *    the name string, but this is confirmed here rather than assumed).
 *  - dt_remote_curve_list_schema(): four schema entries, all
 *    DT_REMOTE_WRITABLE_CONDITIONAL, all three interpolations allowed,
 *    default MONOTONE_HERMITE, non-periodic.
 *  - dt_remote_curve_read_values(): automatic/manual mode gates
 *    active/writable_now correctly for all four always-present semantic
 *    IDs; channel 0 round-trips under curve.master in automatic mode and
 *    under curve.red in manual mode; all three native interpolation ints
 *    map to the right name; native capacity beyond curve_num_nodes[ch] is
 *    never serialized; an out-of-range native curve_type value fails the
 *    whole call closed with DT_REMOTE_ERR_INTERNAL rather than silently
 *    substituting the descriptor's default interpolation.
 *
 * Please see README.md for more detailed documentation.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_curve.h"
#include "control/remote_edit.h"
#include "control/remote_parameters.h"

#include "develop/imageop.h" // dt_iop_get_module_so()/dt_iop_module_so_t/dt_iop_module_t

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* harness: real rgbcurve module .so via dt_init() (mirrors               */
/* test_remote_curve.c's harness_group_setup/teardown exactly)             */
/* ---------------------------------------------------------------------- */

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_harness_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_harness_confdir = g_dir_make_tmp("test_remote_curve_registry-XXXXXX", &gerror);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_curve_registry: failed to create scratch config dir: %s\n",
            gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_curve_registry",
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
      fprintf(stderr, "test_remote_curve_registry: failed to remove scratch config dir %s\n",
              s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_registry_lookup                                         */
/* ---------------------------------------------------------------------- */

static void test_registry_lookup_returns_rgbcurve_adapter_for_v1(void **state)
{
  (void)state;
  const dt_remote_curve_module_adapter_t *adapter = dt_remote_curve_registry_lookup("rgbcurve", 1);
  assert_non_null(adapter);
  assert_string_equal(adapter->operation, "rgbcurve");
  assert_int_equal(adapter->curve_count, 4);
  assert_non_null(adapter->prepare);
  assert_non_null(adapter->validate_completed);
}

static void test_registry_lookup_returns_null_for_unknown_op(void **state)
{
  (void)state;
  assert_null(dt_remote_curve_registry_lookup("this_op_does_not_exist", 1));
}

static void test_registry_lookup_returns_null_for_wrong_version(void **state)
{
  (void)state;
  assert_null(dt_remote_curve_registry_lookup("rgbcurve", 2));
  assert_null(dt_remote_curve_registry_lookup("rgbcurve", 0));
}

static void test_registry_lookup_rejects_null_operation(void **state)
{
  (void)state;
  assert_null(dt_remote_curve_registry_lookup(NULL, 1));
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_registry_validate -- real rgbcurve introspection        */
/* ---------------------------------------------------------------------- */

static void test_registry_validate_passes_against_real_rgbcurve_introspection(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  const dt_remote_curve_module_adapter_t *adapter = dt_remote_curve_registry_lookup("rgbcurve", 1);
  assert_non_null(adapter);

  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_registry_validate(adapter, intro, &err));
  assert_null(err);
}

static void test_registry_resolves_manual_rgb_enum_name(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *fixture = g_malloc0(intro->size);
  dt_introspection_field_t *field = NULL;
  void *ptr = dt_introspection_get_child(intro->field, fixture, "curve_autoscale", &field);
  assert_non_null(ptr);
  assert_non_null(field);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_ENUM);

  const char *name = dt_introspection_get_enum_name(field, 1);
  assert_non_null(name);
  assert_string_equal(name, "DT_S_SCALE_MANUAL_RGB");

  int value = -1;
  assert_true(dt_introspection_get_enum_value(field, "DT_S_SCALE_MANUAL_RGB", &value));
  assert_int_equal(value, 1);

  g_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_registry_validate -- shape mismatch + caching, using a  */
/* private descriptor/adapter/introspection fixture entirely independent  */
/* of the shared "rgbcurve" adapter/cache slot used by the tests above.    */
/* ---------------------------------------------------------------------- */

static const dt_remote_path_segment_t s_priv_nodes_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "nodes" },
};
static const dt_remote_introspection_path_t s_priv_nodes_path = {
  .segments = s_priv_nodes_segments, .length = G_N_ELEMENTS(s_priv_nodes_segments)
};
static const dt_remote_path_segment_t s_priv_count_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "count" },
};
static const dt_remote_introspection_path_t s_priv_count_path = {
  .segments = s_priv_count_segments, .length = G_N_ELEMENTS(s_priv_count_segments)
};
static const dt_remote_path_segment_t s_priv_type_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "type" },
};
static const dt_remote_introspection_path_t s_priv_type_path = {
  .segments = s_priv_type_segments, .length = G_N_ELEMENTS(s_priv_type_segments)
};
static const dt_remote_introspection_path_t s_priv_no_path = { .segments = NULL, .length = 0 };

static const dt_remote_curve_descriptor_t s_priv_descriptor = {
  .name = "test.private",
  .display_name_msgid = "private",
  .description_msgid = NULL,
  .native = { .nodes = s_priv_nodes_path,
             .count = s_priv_count_path,
             .type = s_priv_type_path,
             .x_field = "x",
             .y_field = "y",
             .internal_version = s_priv_no_path,
             .internal_version_value = 0 },
  .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
  .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
  .minimum_points = 2,
  .maximum_points = 4,
  .minimum_x_spacing = 0.0,
  .adjacent_spacing_rule = DT_REMOTE_SPACING_NONE,
  .minimum_wrap_spacing = 0.0,
  .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
  .strict_x_order = TRUE,
  .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
  .interpolation_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE),
  .default_interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE,
  .active_when = NULL,
  .writable_when = NULL,
  .periodic_when = NULL,
};

static gboolean priv_prepare_stub(const struct dt_remote_curve_context_t *ctx, const void *old_params,
                                  void *new_params, const dt_remote_patch_t *patch,
                                  dt_remote_error_t **error)
{
  (void)ctx; (void)old_params; (void)new_params; (void)patch; (void)error;
  return TRUE;
}

static gboolean priv_validate_completed_stub(const struct dt_remote_curve_context_t *ctx,
                                             const void *new_params, dt_remote_error_t **error)
{
  (void)ctx; (void)new_params; (void)error;
  return TRUE;
}

static const dt_remote_curve_module_adapter_t s_priv_adapter = {
  .operation = "test.private_op",
  .minimum_params_version = 1,
  .maximum_params_version = 1,
  .curves = &s_priv_descriptor,
  .curve_count = 1,
  .prepare_fields = NULL,
  .prepare_field_count = 0,
  .prepare = priv_prepare_stub,
  .validate_completed = priv_validate_completed_stub,
};

// "good" tree: nodes is an array of {x,y} float structs (the correct shape).
static dt_introspection_field_t s_priv_leaf_x = {
  .Float = { .header = { .type = DT_INTROSPECTION_TYPE_FLOAT, .type_name = "float", .name = "nodes.x",
                        .field_name = "x", .description = "", .size = sizeof(float), .offset = 0, .so = NULL },
            .Min = 0.0f, .Max = 1.0f, .Default = 0.0f }
};
static dt_introspection_field_t s_priv_leaf_y = {
  .Float = { .header = { .type = DT_INTROSPECTION_TYPE_FLOAT, .type_name = "float", .name = "nodes.y",
                        .field_name = "y", .description = "", .size = sizeof(float), .offset = sizeof(float),
                        .so = NULL },
            .Min = 0.0f, .Max = 1.0f, .Default = 0.0f }
};
static dt_introspection_field_t *s_priv_node_struct_fields[] = { &s_priv_leaf_x, &s_priv_leaf_y, NULL };
static dt_introspection_field_t s_priv_node_struct = {
  .Struct = { .header = { .type = DT_INTROSPECTION_TYPE_STRUCT, .type_name = "node", .name = "nodes[]",
                         .field_name = "nodes[]", .description = "", .size = sizeof(float) * 2, .offset = 0,
                         .so = NULL },
             .entries = 2, .fields = s_priv_node_struct_fields }
};
static dt_introspection_field_t s_priv_good_nodes_array = {
  .Array = { .header = { .type = DT_INTROSPECTION_TYPE_ARRAY, .type_name = "node[4]", .name = "nodes",
                        .field_name = "nodes", .description = "", .size = sizeof(float) * 2 * 4, .offset = 0,
                        .so = NULL },
            .count = 4, .type = DT_INTROSPECTION_TYPE_STRUCT, .field = &s_priv_node_struct }
};

// "bad" tree: nodes is an array of plain ints -- the wrong element type.
static dt_introspection_field_t s_priv_int_leaf = {
  .Int = { .header = { .type = DT_INTROSPECTION_TYPE_INT, .type_name = "int", .name = "leaf",
                      .field_name = "leaf", .description = "", .size = sizeof(int), .offset = 0, .so = NULL },
          .Min = G_MININT, .Max = G_MAXINT, .Default = 0 }
};
static dt_introspection_field_t s_priv_bad_nodes_array = {
  .Array = { .header = { .type = DT_INTROSPECTION_TYPE_ARRAY, .type_name = "int[4]", .name = "nodes",
                        .field_name = "nodes", .description = "", .size = sizeof(int) * 4, .offset = 0,
                        .so = NULL },
            .count = 4, .type = DT_INTROSPECTION_TYPE_INT, .field = &s_priv_int_leaf }
};

static dt_introspection_field_t s_priv_count_field = {
  .Int = { .header = { .type = DT_INTROSPECTION_TYPE_INT, .type_name = "int", .name = "count",
                      .field_name = "count", .description = "", .size = sizeof(int), .offset = 0, .so = NULL },
          .Min = 0, .Max = 100, .Default = 0 }
};
static dt_introspection_field_t s_priv_type_field = {
  .Int = { .header = { .type = DT_INTROSPECTION_TYPE_INT, .type_name = "int", .name = "type",
                      .field_name = "type", .description = "", .size = sizeof(int), .offset = 0, .so = NULL },
          .Min = 0, .Max = 2, .Default = 0 }
};

static dt_introspection_field_t *s_priv_good_struct_fields[] = {
  &s_priv_good_nodes_array, &s_priv_count_field, &s_priv_type_field, NULL
};
static dt_introspection_field_t s_priv_good_root = {
  .Struct = { .header = { .type = DT_INTROSPECTION_TYPE_STRUCT, .type_name = "root", .name = "",
                         .field_name = "", .description = "", .size = 0, .offset = 0, .so = NULL },
             .entries = 3, .fields = s_priv_good_struct_fields }
};

static dt_introspection_field_t *s_priv_bad_struct_fields[] = {
  &s_priv_bad_nodes_array, &s_priv_count_field, &s_priv_type_field, NULL
};
static dt_introspection_field_t s_priv_bad_root = {
  .Struct = { .header = { .type = DT_INTROSPECTION_TYPE_STRUCT, .type_name = "root", .name = "",
                         .field_name = "", .description = "", .size = 0, .offset = 0, .so = NULL },
             .entries = 3, .fields = s_priv_bad_struct_fields }
};

static dt_introspection_t s_priv_good_intro = {
  .api_version = DT_INTROSPECTION_VERSION, .params_version = 1, .type_name = "root", .size = 0,
  .field = &s_priv_good_root, .self_size = 0, .default_params = 0
};
static dt_introspection_t s_priv_bad_intro = {
  .api_version = DT_INTROSPECTION_VERSION, .params_version = 1, .type_name = "root", .size = 0,
  .field = &s_priv_bad_root, .self_size = 0, .default_params = 0
};

static void test_registry_validate_rejects_shape_mismatch_and_caches_result(void **state)
{
  (void)state;
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_registry_validate(&s_priv_adapter, &s_priv_bad_intro, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
  err = NULL;

  // Cached: a second call for the SAME adapter, now given the *good*
  // introspection, still returns the first (failing) result -- proving
  // the shape check ran once and was cached, not re-run per call.
  assert_false(dt_remote_curve_registry_validate(&s_priv_adapter, &s_priv_good_intro, &err));
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(err);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_list_schema                                             */
/* ---------------------------------------------------------------------- */

static void test_list_schema_returns_four_conditional_entries(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);

  GPtrArray *schemas = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_list_schema(so, &schemas, &err));
  assert_null(err);
  assert_non_null(schemas);
  assert_int_equal(schemas->len, 4);

  gboolean saw_master = FALSE, saw_red = FALSE, saw_green = FALSE, saw_blue = FALSE;
  for(guint i = 0; i < schemas->len; i++)
  {
    dt_remote_curve_schema_t *schema = g_ptr_array_index(schemas, i);
    assert_int_equal(schema->writability, DT_REMOTE_WRITABLE_CONDITIONAL);
    assert_false(schema->periodic_x);
    assert_null(schema->periodic_when);
    assert_non_null(schema->writable_when);
    assert_non_null(schema->active_when);
    assert_int_equal(schema->default_interpolation, DT_REMOTE_CURVE_MONOTONE_HERMITE);
    const guint expected_mask = (1u << DT_REMOTE_CURVE_CUBIC_SPLINE) | (1u << DT_REMOTE_CURVE_CATMULL_ROM)
                              | (1u << DT_REMOTE_CURVE_MONOTONE_HERMITE);
    assert_int_equal(schema->interpolation_mask, expected_mask);
    assert_int_equal(schema->minimum_points, 2);
    assert_int_equal(schema->maximum_points, 20);

    if(!g_strcmp0(schema->name, "curve.master")) saw_master = TRUE;
    else if(!g_strcmp0(schema->name, "curve.red")) saw_red = TRUE;
    else if(!g_strcmp0(schema->name, "curve.green")) saw_green = TRUE;
    else if(!g_strcmp0(schema->name, "curve.blue")) saw_blue = TRUE;
  }
  assert_true(saw_master);
  assert_true(saw_red);
  assert_true(saw_green);
  assert_true(saw_blue);

  g_ptr_array_unref(schemas);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_curve_read_values                                             */
/* ---------------------------------------------------------------------- */

static void init_fake_module(dt_iop_module_t *module, dt_iop_module_so_t *so)
{
  memset(module, 0, sizeof(*module));
  module->so = so;
  g_strlcpy(module->op, so->op, sizeof(module->op));
}

static void resolve_field_or_fail(dt_introspection_t *intro, void *blob,
                                  const dt_remote_path_segment_t *segments, guint length,
                                  const dt_introspection_field_t **out_field, void **out_ptr)
{
  const dt_remote_introspection_path_t path = { .segments = segments, .length = length };
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_path_resolve(&path, intro->field, blob, out_field, out_ptr, &err));
  assert_null(err);
}

static void set_curve_autoscale(dt_introspection_t *intro, void *blob, int value)
{
  const dt_remote_path_segment_t segments[] = {
    { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_autoscale" },
  };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  resolve_field_or_fail(intro, blob, segments, G_N_ELEMENTS(segments), &field, &ptr);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_ENUM);
  *(int *)ptr = value;
}

static void set_curve_num_nodes(dt_introspection_t *intro, void *blob, guint channel, int count)
{
  const dt_remote_path_segment_t segments[] = {
    { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_num_nodes" },
    { .type = DT_REMOTE_PATH_INDEX, .value.index = channel },
  };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  resolve_field_or_fail(intro, blob, segments, G_N_ELEMENTS(segments), &field, &ptr);
  *(int *)ptr = count;
}

static void set_curve_type(dt_introspection_t *intro, void *blob, guint channel, int type)
{
  const dt_remote_path_segment_t segments[] = {
    { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_type" },
    { .type = DT_REMOTE_PATH_INDEX, .value.index = channel },
  };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  resolve_field_or_fail(intro, blob, segments, G_N_ELEMENTS(segments), &field, &ptr);
  *(int *)ptr = type;
}

static void set_curve_node(dt_introspection_t *intro, void *blob, guint channel, guint node,
                           double x, double y)
{
  dt_remote_path_segment_t segments[] = {
    { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
    { .type = DT_REMOTE_PATH_INDEX, .value.index = channel },
    { .type = DT_REMOTE_PATH_INDEX, .value.index = node },
    { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  resolve_field_or_fail(intro, blob, segments, G_N_ELEMENTS(segments), &field, &ptr);
  assert_int_equal(field->header.type, DT_INTROSPECTION_TYPE_FLOAT);
  *(float *)ptr = (float)x;

  segments[3].value.field = "y";
  resolve_field_or_fail(intro, blob, segments, G_N_ELEMENTS(segments), &field, &ptr);
  *(float *)ptr = (float)y;
}

// DT_S_SCALE_AUTOMATIC_RGB / DT_S_SCALE_MANUAL_RGB (src/iop/rgbcurve.c).
#define AUTOMATIC_RGB 0
#define MANUAL_RGB 1

static void test_read_values_automatic_mode_exposes_master_only(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, AUTOMATIC_RGB);
  set_curve_num_nodes(intro, blob, 0, 2);
  set_curve_type(intro, blob, 0, 2); // MONOTONE_HERMITE
  set_curve_node(intro, blob, 0, 0, 0.0, 0.0);
  set_curve_node(intro, blob, 0, 1, 1.0, 1.0);

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(err);
  assert_non_null(values);
  assert_int_equal(g_hash_table_size(values), 4);

  dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
  assert_non_null(master);
  assert_true(master->active);
  assert_true(master->effective);
  assert_true(master->writable_now);
  assert_false(master->periodic_x);

  dt_remote_curve_value_t *red = g_hash_table_lookup(values, "curve.red");
  assert_non_null(red);
  assert_false(red->active);
  assert_false(red->writable_now);

  dt_remote_curve_value_t *green = g_hash_table_lookup(values, "curve.green");
  assert_non_null(green);
  assert_false(green->active);

  dt_remote_curve_value_t *blue = g_hash_table_lookup(values, "curve.blue");
  assert_non_null(blue);
  assert_false(blue->active);

  g_hash_table_unref(values);
  g_free(blob);
}

static void test_read_values_manual_mode_exposes_rgb_only(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, MANUAL_RGB);
  for(guint ch = 0; ch < 3; ch++)
  {
    set_curve_num_nodes(intro, blob, ch, 2);
    set_curve_type(intro, blob, ch, 2);
    set_curve_node(intro, blob, ch, 0, 0.0, 0.0);
    set_curve_node(intro, blob, ch, 1, 1.0, 1.0);
  }

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(err);
  assert_int_equal(g_hash_table_size(values), 4);

  dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
  assert_non_null(master);
  assert_false(master->active);
  assert_false(master->writable_now);

  dt_remote_curve_value_t *red = g_hash_table_lookup(values, "curve.red");
  assert_non_null(red);
  assert_true(red->active);
  assert_true(red->writable_now);

  dt_remote_curve_value_t *green = g_hash_table_lookup(values, "curve.green");
  assert_non_null(green);
  assert_true(green->active);
  assert_true(green->writable_now);

  dt_remote_curve_value_t *blue = g_hash_table_lookup(values, "curve.blue");
  assert_non_null(blue);
  assert_true(blue->active);
  assert_true(blue->writable_now);

  g_hash_table_unref(values);
  g_free(blob);
}

static void test_read_values_channel0_round_trips_under_master_in_automatic_mode(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, AUTOMATIC_RGB);
  set_curve_num_nodes(intro, blob, 0, 3);
  set_curve_type(intro, blob, 0, 2);
  set_curve_node(intro, blob, 0, 0, 0.0, 0.1);
  set_curve_node(intro, blob, 0, 1, 0.5, 0.6);
  set_curve_node(intro, blob, 0, 2, 1.0, 0.9);

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(err);

  dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
  assert_non_null(master);
  assert_non_null(master->points);
  assert_int_equal(master->points->len, 3);

  dt_remote_curve_point_t *p0 = &g_array_index(master->points, dt_remote_curve_point_t, 0);
  dt_remote_curve_point_t *p1 = &g_array_index(master->points, dt_remote_curve_point_t, 1);
  dt_remote_curve_point_t *p2 = &g_array_index(master->points, dt_remote_curve_point_t, 2);
  assert_float_equal(p0->x, 0.0, 1e-6); assert_float_equal(p0->y, 0.1, 1e-6);
  assert_float_equal(p1->x, 0.5, 1e-6); assert_float_equal(p1->y, 0.6, 1e-6);
  assert_float_equal(p2->x, 1.0, 1e-6); assert_float_equal(p2->y, 0.9, 1e-6);

  g_hash_table_unref(values);
  g_free(blob);
}

static void test_read_values_channel0_round_trips_under_red_in_manual_mode(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, MANUAL_RGB);
  set_curve_num_nodes(intro, blob, 0, 2);
  set_curve_type(intro, blob, 0, 0);
  set_curve_node(intro, blob, 0, 0, 0.2, 0.3);
  set_curve_node(intro, blob, 0, 1, 0.8, 0.4);
  // other channels need at least a well-formed 2-point curve too, though
  // this test only inspects channel 0's semantic ID under this mode.
  for(guint ch = 1; ch < 3; ch++)
  {
    set_curve_num_nodes(intro, blob, ch, 2);
    set_curve_type(intro, blob, ch, 0);
    set_curve_node(intro, blob, ch, 0, 0.0, 0.0);
    set_curve_node(intro, blob, ch, 1, 1.0, 1.0);
  }

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(err);

  // channel 0's storage now surfaces as curve.red (manual mode), not
  // curve.master.
  dt_remote_curve_value_t *red = g_hash_table_lookup(values, "curve.red");
  assert_non_null(red);
  assert_true(red->active);
  assert_non_null(red->points);
  assert_int_equal(red->points->len, 2);
  dt_remote_curve_point_t *p0 = &g_array_index(red->points, dt_remote_curve_point_t, 0);
  dt_remote_curve_point_t *p1 = &g_array_index(red->points, dt_remote_curve_point_t, 1);
  assert_float_equal(p0->x, 0.2, 1e-6); assert_float_equal(p0->y, 0.3, 1e-6);
  assert_float_equal(p1->x, 0.8, 1e-6); assert_float_equal(p1->y, 0.4, 1e-6);
  assert_int_equal(red->interpolation, DT_REMOTE_CURVE_CUBIC_SPLINE);

  dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
  assert_non_null(master);
  assert_false(master->active);

  g_hash_table_unref(values);
  g_free(blob);
}

static void test_read_values_interpolation_maps_all_three_names(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  const struct { int native_value; dt_remote_curve_interpolation_t expected; } cases[] = {
    { 0, DT_REMOTE_CURVE_CUBIC_SPLINE },
    { 1, DT_REMOTE_CURVE_CATMULL_ROM },
    { 2, DT_REMOTE_CURVE_MONOTONE_HERMITE },
  };

  for(guint i = 0; i < G_N_ELEMENTS(cases); i++)
  {
    void *blob = g_malloc0(intro->size);
    set_curve_autoscale(intro, blob, AUTOMATIC_RGB);
    set_curve_num_nodes(intro, blob, 0, 2);
    set_curve_type(intro, blob, 0, cases[i].native_value);
    set_curve_node(intro, blob, 0, 0, 0.0, 0.0);
    set_curve_node(intro, blob, 0, 1, 1.0, 1.0);

    dt_iop_module_t module;
    init_fake_module(&module, so);

    GHashTable *values = NULL;
    dt_remote_error_t *err = NULL;
    assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
    assert_null(err);

    dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
    assert_non_null(master);
    assert_int_equal(master->interpolation, cases[i].expected);

    g_hash_table_unref(values);
    g_free(blob);
  }
}

static void test_read_values_unused_capacity_not_serialized(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, AUTOMATIC_RGB);
  set_curve_num_nodes(intro, blob, 0, 3);
  set_curve_type(intro, blob, 0, 2);
  set_curve_node(intro, blob, 0, 0, 0.0, 0.1);
  set_curve_node(intro, blob, 0, 1, 0.5, 0.5);
  set_curve_node(intro, blob, 0, 2, 1.0, 0.9);
  // garbage beyond the active count -- DT_IOP_RGBCURVE_MAXNODES is 20.
  for(guint node = 3; node < 20; node++)
    set_curve_node(intro, blob, 0, node, 0.777, 0.777);

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_true(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(err);

  dt_remote_curve_value_t *master = g_hash_table_lookup(values, "curve.master");
  assert_non_null(master);
  assert_int_equal(master->points->len, 3);
  for(guint i = 0; i < master->points->len; i++)
  {
    dt_remote_curve_point_t *p = &g_array_index(master->points, dt_remote_curve_point_t, i);
    assert_true(p->x != 0.777);
    assert_true(p->y != 0.777);
  }

  g_hash_table_unref(values);
  g_free(blob);
}

static void test_read_values_out_of_range_curve_type_fails_closed(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("rgbcurve");
  assert_non_null(so);
  dt_introspection_t *intro = so->get_introspection();
  assert_non_null(intro);

  void *blob = g_malloc0(intro->size);
  set_curve_autoscale(intro, blob, AUTOMATIC_RGB);
  set_curve_num_nodes(intro, blob, 0, 2);
  set_curve_type(intro, blob, 0, 99); // out of range: none of 0/1/2
  set_curve_node(intro, blob, 0, 0, 0.0, 0.0);
  set_curve_node(intro, blob, 0, 1, 1.0, 1.0);

  dt_iop_module_t module;
  init_fake_module(&module, so);

  GHashTable *values = NULL;
  dt_remote_error_t *err = NULL;
  assert_false(dt_remote_curve_read_values(&module, blob, &values, &err));
  assert_null(values);
  assert_non_null(err);
  assert_int_equal(err->code, DT_REMOTE_ERR_INTERNAL);
  assert_non_null(err->message);
  assert_true(strlen(err->message) > 0);

  dt_remote_error_free(err);
  g_free(blob);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_registry_lookup_returns_rgbcurve_adapter_for_v1),
    cmocka_unit_test(test_registry_lookup_returns_null_for_unknown_op),
    cmocka_unit_test(test_registry_lookup_returns_null_for_wrong_version),
    cmocka_unit_test(test_registry_lookup_rejects_null_operation),
    cmocka_unit_test(test_registry_validate_passes_against_real_rgbcurve_introspection),
    cmocka_unit_test(test_registry_resolves_manual_rgb_enum_name),
    cmocka_unit_test(test_registry_validate_rejects_shape_mismatch_and_caches_result),
    cmocka_unit_test(test_list_schema_returns_four_conditional_entries),
    cmocka_unit_test(test_read_values_automatic_mode_exposes_master_only),
    cmocka_unit_test(test_read_values_manual_mode_exposes_rgb_only),
    cmocka_unit_test(test_read_values_channel0_round_trips_under_master_in_automatic_mode),
    cmocka_unit_test(test_read_values_channel0_round_trips_under_red_in_manual_mode),
    cmocka_unit_test(test_read_values_interpolation_maps_all_three_names),
    cmocka_unit_test(test_read_values_unused_capacity_not_serialized),
    cmocka_unit_test(test_read_values_out_of_range_curve_type_fails_closed),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
