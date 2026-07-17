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
 * cmocka unit tests for the quantity-class engine and registry core
 * (src/control/remote_quantity.c/.h, src/control/remote_quantity_registry.c),
 * mirroring test_remote_band.c's structure and fixture idioms for the band
 * twin. The engine sections below predate any shipped adapter -- every
 * engine behavior is proven with a test-local, hand-rolled adapter/
 * descriptor against the real loaded `temperature` .so, installed through
 * dt_remote_quantity_registry_set_lookup_override() (the same test seam
 * dt_remote_band_registry_set_lookup_override() provides for the band
 * registry), plus a fake conversion-hook pair installed through
 * dt_remote_quantity_set_hooks_override() -- the class's own new seam,
 * needed because the real temperature hooks fail closed without a built
 * GUI (temperature.c's remote_quantity_read/write both require
 * `self->gui_data`, which the unit-test fixture never builds; see
 * test_remote_band.c:687-690's own module-loading fixture, reused here
 * unchanged for a "no image" `dt_iop_load_module`).
 *
 * The test-local descriptor ("test.quantity_pair", components
 * "component_a"/"component_b") is deliberately named differently from the
 * milestone's real adapter ("wb.temperature", components "temperature"/
 * "tint", Task 5) even though both target the "temperature" op: the
 * registry lookup override fully replaces the (currently empty) production
 * table for the duration of each test, so there is no runtime collision,
 * but a different name avoids any reader confusion once Task 5 appends its
 * own per-adapter section to this same file. The fake hook pair reads/
 * writes the `red`/`green` native float scalars (well within
 * `native_fields = {"red","green","blue","various"}`) through a simple,
 * invertible affine transform, so success-path tests can predict exact
 * round-trip values.
 *
 * `component_b`'s domain deliberately reuses the real spec's Kelvin upper
 * bound (25000.0) so the double-precision boundary test
 * (25000.00001 rejected, 25000.0 accepted) exercises the same numeric edge
 * the real wb.temperature adapter will have.
 *
 * Coverage:
 *  - Task 2 presence checks: temperature's so exports both hooks, exposure's
 *    does not.
 *  - dt_remote_quantity_schema_free()/dt_remote_quantity_value_free() on
 *    fully populated, partially populated, and NULL instances.
 *  - dt_remote_quantity_validate(): pure, exact component name-set
 *    (missing/unknown/duplicate), finiteness, domain (including the
 *    double-precision boundary).
 *  - dt_remote_quantity_registry_validate(): adapter envelope, component
 *    metadata (count cap, minimum/maximum ordering), duplicate descriptor
 *    names, cross-class collisions with the curve, vector, AND band
 *    registries, native-field resolution (unresolvable name, wrong type/
 *    denylisted), hook presence (fails closed without an override).
 *  - dt_remote_quantity_list_schema(): descriptor -> owned schema
 *    conversion in registry order; no-adapter silent degrade (NULL, not
 *    empty array).
 *  - dt_remote_quantity_read_values(): hook-driven read, active/
 *    writable_now stamped TRUE for NULL predicates, hook-failure fails
 *    closed.
 *  - dt_remote_quantity_apply_entries()/_apply_patch(): unknown ID,
 *    duplicate ID, validation-per-requirement-4 rejections (each leaving
 *    the scratch block untouched), the native-conflict rule (scalar write
 *    to a `native_fields` name + a quantity entry in the same patch), a
 *    successful write that leaves every byte outside `native_fields`
 *    untouched, a stray-byte write hook failing the byte-check and rolling
 *    back, a write-hook failure (both a scripted FALSE and the real
 *    no-GUI-fails-closed path) rolling back, a full apply -> read
 *    round-trip through the fake hooks, and a mixed-class patch silently
 *    skipping a non-quantity entry.
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
#include "control/remote_quantity.h"
#include "control/remote_vector.h"

#include "develop/develop.h"
#include "develop/imageop.h" // dt_iop_get_module_so()/dt_iop_module_so_t/dt_iop_module_t

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_schema_free / dt_remote_quantity_value_free           */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *make_condition(const char *field, const char *enum_name)
{
  dt_remote_parameter_condition_t *c = g_new0(dt_remote_parameter_condition_t, 1);
  c->field = g_strdup(field);
  c->op = DT_REMOTE_PREDICATE_EQ;
  c->enum_name = g_strdup(enum_name);
  return c;
}

static dt_remote_quantity_schema_component_t *make_schema_component(const char *name, const char *unit,
                                                                    double minimum, double maximum)
{
  dt_remote_quantity_schema_component_t *component = g_new0(dt_remote_quantity_schema_component_t, 1);
  component->name = g_strdup(name);
  component->unit = unit ? g_strdup(unit) : NULL;
  component->minimum = minimum;
  component->maximum = maximum;
  return component;
}

static void test_schema_free_fully_populated(void **state)
{
  (void)state;
  dt_remote_quantity_schema_t *schema = g_new0(dt_remote_quantity_schema_t, 1);
  schema->name = g_strdup("test.quantity_pair");
  schema->display_name = g_strdup("Test Pair");
  schema->description = g_strdup("a synthetic quantity");
  schema->components = g_ptr_array_new();
  g_ptr_array_add(schema->components, make_schema_component("component_a", NULL, -50.0, 50.0));
  g_ptr_array_add(schema->components, make_schema_component("component_b", "kelvin", 0.0, 25000.0));
  schema->derived = TRUE;
  schema->writability = DT_REMOTE_WRITABLE_CONDITIONAL;
  schema->active_when = make_condition("mode", "MODE_A");
  schema->writable_when = make_condition("mode", "MODE_B");

  dt_remote_quantity_schema_free(schema); // must not crash/leak (checked under valgrind/asan in CI)
}

static void test_schema_free_minimal(void **state)
{
  (void)state;
  dt_remote_quantity_schema_t *schema = g_new0(dt_remote_quantity_schema_t, 1);
  schema->name = g_strdup("test.quantity_pair");
  schema->display_name = g_strdup("Test Pair");
  // description, components, active_when, writable_when all left NULL/zero.
  dt_remote_quantity_schema_free(schema);
}

static void test_schema_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_quantity_schema_free(NULL);
}

static void test_value_free_fully_populated(void **state)
{
  (void)state;
  dt_remote_quantity_value_t *value = g_new0(dt_remote_quantity_value_t, 1);
  value->name = g_strdup("test.quantity_pair");
  value->values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_quantity_component_value_free);
  dt_remote_quantity_component_value_t *a = g_new0(dt_remote_quantity_component_value_t, 1);
  a->name = g_strdup("component_a");
  a->value = 1.5;
  g_ptr_array_add(value->values, a);
  value->active = TRUE;
  value->effective = TRUE;
  value->writable_now = TRUE;

  dt_remote_quantity_value_free(value);
}

static void test_value_free_null_is_safe(void **state)
{
  (void)state;
  dt_remote_quantity_value_free(NULL);
}

/* ---------------------------------------------------------------------- */
/* harness: real "temperature"/"exposure" module .so's via dt_init()        */
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
  s_harness_confdir = g_dir_make_tmp("test_remote_quantity-XXXXXX", &gerror);
  if(!s_harness_confdir)
  {
    fprintf(stderr, "test_remote_quantity: failed to create scratch config dir: %s\n", gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_quantity",
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
      fprintf(stderr, "test_remote_quantity: failed to remove scratch config dir %s\n", s_harness_confdir);
    g_free(cmd);
    g_free(s_harness_confdir);
    s_harness_confdir = NULL;
  }
  return 0;
}

static int lookup_override_test_setup(void **state)
{
  (void)state;
  dt_remote_quantity_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  dt_remote_vector_registry_set_lookup_override(NULL);
  dt_remote_band_registry_set_lookup_override(NULL);
  dt_remote_quantity_set_hooks_override(NULL, NULL);
  return 0;
}

static int lookup_override_test_teardown(void **state)
{
  (void)state;
  dt_remote_quantity_registry_set_lookup_override(NULL);
  dt_remote_curve_registry_set_lookup_override(NULL);
  dt_remote_vector_registry_set_lookup_override(NULL);
  dt_remote_band_registry_set_lookup_override(NULL);
  dt_remote_quantity_set_hooks_override(NULL, NULL);
  return 0;
}

/* ---------------------------------------------------------------------- */
/* fixture: descriptor/adapter data, module loading                        */
/* ---------------------------------------------------------------------- */

typedef struct quantity_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} quantity_fixture_t;

// Generic real-module loader, same "adapter_fixture_new(op)" idiom
// test_remote_band.c's real_band_module_fixture_new() uses. No GUI is
// built (fixture->dev.gui_attached = FALSE), which is exactly why the real
// temperature remote_quantity_read/write hooks fail closed here -- see
// this file's own top comment.
static quantity_fixture_t *real_quantity_module_fixture_new(const char *op)
{
  quantity_fixture_t *fixture = g_new0(quantity_fixture_t, 1);
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

static quantity_fixture_t *temperature_fixture_new(void)
{
  return real_quantity_module_fixture_new("temperature");
}

static void quantity_fixture_free(quantity_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static void *scratch_params_new(dt_iop_module_t *module)
{
  void *scratch = g_malloc(module->params_size);
  memcpy(scratch, module->params, module->params_size);
  return scratch;
}

// Resolves `field_name` as a single-segment introspection path against
// `module` and pokes/reads a native float scalar directly -- test-only
// helper bypassing the engine entirely, same rationale as
// test_remote_band.c's poke_band_array()/read_band_array().
static void poke_temperature_float(dt_iop_module_t *module, const char *field_name, void *params_blob,
                                   float value)
{
  dt_introspection_t *intro = module->so->get_introspection();
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = field_name };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  assert_true(dt_remote_path_resolve(&path, intro->field, params_blob, &field, &ptr, NULL));
  *(float *)ptr = value;
}

static float read_temperature_float(dt_iop_module_t *module, const char *field_name,
                                    const void *params_blob)
{
  dt_introspection_t *intro = module->so->get_introspection();
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = field_name };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  assert_true(dt_remote_path_resolve(&path, intro->field, (void *)params_blob, &field, &ptr, NULL));
  return *(float *)ptr;
}

/* ---------------------------------------------------------------------- */
/* fixture: test-local "test.quantity_pair" descriptor/adapter on          */
/* "temperature" (params v4)                                               */
/* ---------------------------------------------------------------------- */

#define QUANTITY_TEST_A_MIN (-50.0)
#define QUANTITY_TEST_A_MAX (50.0)
#define QUANTITY_TEST_B_MIN (0.0)
#define QUANTITY_TEST_B_MAX (25000.0) // deliberately matches the real spec's Kelvin bound

static const dt_remote_quantity_component_descriptor_t s_test_components[2] = {
  { "component_a", NULL, QUANTITY_TEST_A_MIN, QUANTITY_TEST_A_MAX },
  { "component_b", "kelvin", QUANTITY_TEST_B_MIN, QUANTITY_TEST_B_MAX },
};

static const char *const s_test_component_names[2] = { "component_a", "component_b" };

// All four of temperature's coefficient scalars -- real writable float
// leaves, none denylisted (only "preset" is, and it's an int, not one of
// these).
static const char *const s_test_native_fields[4] = { "red", "green", "blue", "various" };

static dt_remote_quantity_descriptor_t make_test_descriptor(void)
{
  dt_remote_quantity_descriptor_t desc = { 0 };
  desc.name = "test.quantity_pair";
  desc.display_name = "Test Pair";
  desc.description = "synthetic quantity descriptor for engine tests";
  desc.components = s_test_components;
  desc.component_count = 2;
  desc.derived = TRUE;
  desc.active_when = NULL;
  desc.writable_when = NULL;
  return desc;
}

// Builds a fresh heap-allocated adapter/descriptor pair wrapping
// make_test_descriptor() over "temperature" (params v4), tracked in
// s_registry_test_allocations for teardown -- same idiom as
// test_remote_band.c's new_lowlight_test_adapter().
static dt_remote_quantity_module_adapter_t *new_temperature_test_adapter(
  dt_remote_quantity_descriptor_t **out_descriptor)
{
  dt_remote_quantity_descriptor_t *descriptor = g_new(dt_remote_quantity_descriptor_t, 1);
  *descriptor = make_test_descriptor();

  dt_remote_quantity_module_adapter_t *adapter = g_new0(dt_remote_quantity_module_adapter_t, 1);
  *adapter = (dt_remote_quantity_module_adapter_t){
    .operation = "temperature",
    .minimum_params_version = 4,
    .maximum_params_version = 4,
    .quantities = descriptor,
    .quantity_count = 1,
    .native_fields = s_test_native_fields,
    .native_field_count = 4,
  };

  g_ptr_array_add(s_registry_test_allocations, descriptor);
  g_ptr_array_add(s_registry_test_allocations, adapter);
  if(out_descriptor) *out_descriptor = descriptor;
  return adapter;
}

static void assert_quantity_registry_rejects(const dt_remote_quantity_module_adapter_t *adapter,
                                             const dt_iop_module_so_t *so)
{
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_registry_validate(adapter, so, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

static const dt_remote_quantity_module_adapter_t *s_lookup_override_adapter = NULL;

static const dt_remote_quantity_module_adapter_t *private_quantity_lookup_override(
  const char *operation, guint params_version)
{
  if(!s_lookup_override_adapter || g_strcmp0(operation, s_lookup_override_adapter->operation)
     || params_version < s_lookup_override_adapter->minimum_params_version
     || params_version > s_lookup_override_adapter->maximum_params_version)
    return NULL;
  return s_lookup_override_adapter;
}

static void install_quantity_adapter(const dt_remote_quantity_module_adapter_t *adapter)
{
  s_lookup_override_adapter = adapter;
  dt_remote_quantity_registry_set_lookup_override(private_quantity_lookup_override);
}

// Synthetic curve/vector/band adapters installed only for the cross-class
// collision tests below; their descriptors are never resolved against real
// introspection (dt_remote_quantity_registry_validate() only reads their
// `.name`), so minimal, unvalidated descriptors are sufficient -- same
// idiom as test_remote_band.c's own colliding-curve/vector fixtures.
static const dt_remote_curve_descriptor_t s_colliding_curve_descriptor = {
  .name = "test.quantity_pair", // deliberately identical to make_test_descriptor()'s name
};
static const dt_remote_curve_module_adapter_t s_colliding_curve_adapter = {
  .operation = "temperature",
  .minimum_params_version = 4,
  .maximum_params_version = 4,
  .curves = &s_colliding_curve_descriptor,
  .curve_count = 1,
};
static const dt_remote_curve_module_adapter_t *colliding_curve_lookup_override(const char *operation,
                                                                                guint params_version)
{
  if(g_strcmp0(operation, "temperature") || params_version != 4) return NULL;
  return &s_colliding_curve_adapter;
}

static const dt_remote_vector_component_t s_colliding_vector_components[1] = { { "v", 0.0, 1.0 } };
static const dt_remote_vector_descriptor_t s_colliding_vector_descriptor = {
  .name = "test.quantity_pair", // deliberately identical to make_test_descriptor()'s name
  .component_count = 1,
  .components = s_colliding_vector_components,
};
static const dt_remote_vector_module_adapter_t s_colliding_vector_adapter = {
  .operation = "temperature",
  .minimum_params_version = 4,
  .maximum_params_version = 4,
  .vectors = &s_colliding_vector_descriptor,
  .vector_count = 1,
};
static const dt_remote_vector_module_adapter_t *colliding_vector_lookup_override(const char *operation,
                                                                                  guint params_version)
{
  if(g_strcmp0(operation, "temperature") || params_version != 4) return NULL;
  return &s_colliding_vector_adapter;
}

static const dt_remote_band_descriptor_t s_colliding_band_descriptor = {
  .name = "test.quantity_pair", // deliberately identical to make_test_descriptor()'s name
  .count = 1,
};
static const dt_remote_band_module_adapter_t s_colliding_band_adapter = {
  .operation = "temperature",
  .minimum_params_version = 4,
  .maximum_params_version = 4,
  .bands = &s_colliding_band_descriptor,
  .band_count = 1,
};
static const dt_remote_band_module_adapter_t *colliding_band_lookup_override(const char *operation,
                                                                              guint params_version)
{
  if(g_strcmp0(operation, "temperature") || params_version != 4) return NULL;
  return &s_colliding_band_adapter;
}

/* ---------------------------------------------------------------------- */
/* fake conversion hooks: reversible affine transform over red/green        */
/* ---------------------------------------------------------------------- */

// component_a = 2 * red; component_b = 100 * green. Both within
// [-FLT_MAX, FLT_MAX] for any value in the descriptor's own domain, and
// trivially invertible so success-path tests can predict exact
// round-trip values.
static gboolean fake_quantity_read_hook(struct dt_iop_module_t *self, const void *params,
                                        double *values, size_t count)
{
  if(!self || !params || !values || count != 2) return FALSE;
  values[0] = (double)read_temperature_float(self, "red", params) * 2.0;
  values[1] = (double)read_temperature_float(self, "green", params) * 100.0;
  return TRUE;
}

static gboolean fake_quantity_write_hook(struct dt_iop_module_t *self, const double *values,
                                         size_t count, void *params)
{
  if(!self || !values || !params || count != 2) return FALSE;
  poke_temperature_float(self, "red", params, (float)(values[0] / 2.0));
  poke_temperature_float(self, "green", params, (float)(values[1] / 100.0));
  return TRUE;
}

// Deliberately buggy: also writes `preset` (an int field NOT in
// `native_fields`), to exercise the post-write byte-check's stray-byte
// detection.
static gboolean stray_byte_quantity_write_hook(struct dt_iop_module_t *self, const double *values,
                                               size_t count, void *params)
{
  if(!fake_quantity_write_hook(self, values, count, params)) return FALSE;
  dt_introspection_t *intro = self->so->get_introspection();
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = "preset" };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!dt_remote_path_resolve(&path, intro->field, params, &field, &ptr, NULL)) return FALSE;
  *(int *)ptr = *(int *)ptr + 1;
  return TRUE;
}

static gboolean always_false_quantity_read_hook(struct dt_iop_module_t *self, const void *params,
                                                double *values, size_t count)
{
  (void)self; (void)params; (void)values; (void)count;
  return FALSE;
}

static gboolean always_false_quantity_write_hook(struct dt_iop_module_t *self, const double *values,
                                                 size_t count, void *params)
{
  (void)self; (void)values; (void)count; (void)params;
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Task 2 presence checks                                                  */
/* ---------------------------------------------------------------------- */

static void test_temperature_so_exports_both_hooks(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  assert_non_null(so);
  assert_non_null(so->remote_quantity_read);
  assert_non_null(so->remote_quantity_write);
}

static void test_exposure_so_exports_neither_hook(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("exposure");
  assert_non_null(so);
  assert_null(so->remote_quantity_read);
  assert_null(so->remote_quantity_write);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_validate                                             */
/* ---------------------------------------------------------------------- */

static GPtrArray *make_component_values(const char *const *names, const double *values, guint n)
{
  GPtrArray *array = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_quantity_component_value_free);
  for(guint i = 0; i < n; i++)
  {
    dt_remote_quantity_component_value_t *v = g_new0(dt_remote_quantity_component_value_t, 1);
    v->name = g_strdup(names[i]);
    v->value = values[i];
    g_ptr_array_add(array, v);
  }
  return array;
}

static void test_validate_accepts_well_formed_values(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  const double values[2] = { 1.0, 5500.0 };
  GPtrArray *v = make_component_values(s_test_component_names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_validate(&desc, v, &error));
  assert_null(error);

  g_ptr_array_unref(v);
}

static void test_validate_rejects_missing_component(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  static const char *const names[1] = { "component_a" };
  const double values[1] = { 1.0 };
  GPtrArray *v = make_component_values(names, values, 1);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(&desc, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(error->details_json);
  assert_non_null(strstr(error->details_json, "\"missing_component\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_b\""));

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

static void test_validate_rejects_unknown_component(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  static const char *const names[2] = { "component_a", "component_c" };
  const double values[2] = { 1.0, 2.0 };
  GPtrArray *v = make_component_values(names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(&desc, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"unknown_component\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_c\""));

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

static void test_validate_rejects_duplicate_component(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  static const char *const names[2] = { "component_a", "component_a" };
  const double values[2] = { 1.0, 2.0 };
  GPtrArray *v = make_component_values(names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(&desc, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"duplicate_component\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_a\""));

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

static void test_validate_rejects_non_finite(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  const double values[2] = { NAN, 1.0 };
  GPtrArray *v = make_component_values(s_test_component_names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(&desc, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"non_finite\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_a\""));

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

static void test_validate_rejects_domain_just_outside_double_boundary(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  const double values[2] = { 1.0, 25000.00001 };
  GPtrArray *v = make_component_values(s_test_component_names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(&desc, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"domain\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_b\""));

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

static void test_validate_accepts_domain_exactly_at_boundary(void **state)
{
  (void)state;
  dt_remote_quantity_descriptor_t desc = make_test_descriptor();
  const double values[2] = { QUANTITY_TEST_A_MIN, QUANTITY_TEST_B_MAX };
  GPtrArray *v = make_component_values(s_test_component_names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_validate(&desc, v, &error));
  assert_null(error);

  g_ptr_array_unref(v);
}

static void test_validate_null_descriptor_fails_internal(void **state)
{
  (void)state;
  const double values[2] = { 1.0, 2.0 };
  GPtrArray *v = make_component_values(s_test_component_names, values, 2);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_validate(NULL, v, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);

  dt_remote_error_free(error);
  g_ptr_array_unref(v);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_registry_validate                                    */
/* ---------------------------------------------------------------------- */

static void test_registry_validate_rejects_invalid_adapter_envelope(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  assert_non_null(so);
  dt_remote_quantity_descriptor_t *descriptor = NULL;

  dt_remote_quantity_module_adapter_t *wrong_operation = new_temperature_test_adapter(&descriptor);
  wrong_operation->operation = "exposure";
  assert_quantity_registry_rejects(wrong_operation, so);

  dt_remote_quantity_module_adapter_t *reversed_versions = new_temperature_test_adapter(&descriptor);
  reversed_versions->minimum_params_version = 5;
  reversed_versions->maximum_params_version = 4;
  assert_quantity_registry_rejects(reversed_versions, so);

  dt_remote_quantity_module_adapter_t *outside_version = new_temperature_test_adapter(&descriptor);
  outside_version->minimum_params_version = 1;
  outside_version->maximum_params_version = 2;
  assert_quantity_registry_rejects(outside_version, so);

  dt_remote_quantity_module_adapter_t *null_quantities = new_temperature_test_adapter(&descriptor);
  null_quantities->quantities = NULL;
  assert_quantity_registry_rejects(null_quantities, so);
}

static void test_registry_validate_rejects_component_count_zero(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);
  descriptor->component_count = 0;
  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_component_count_exceeds_cap(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  static const dt_remote_quantity_component_descriptor_t nine_components[9] = {
    { "c0", NULL, 0.0, 1.0 }, { "c1", NULL, 0.0, 1.0 }, { "c2", NULL, 0.0, 1.0 },
    { "c3", NULL, 0.0, 1.0 }, { "c4", NULL, 0.0, 1.0 }, { "c5", NULL, 0.0, 1.0 },
    { "c6", NULL, 0.0, 1.0 }, { "c7", NULL, 0.0, 1.0 }, { "c8", NULL, 0.0, 1.0 },
  };
  descriptor->components = nine_components;
  descriptor->component_count = 9;
  assert_true(9 > DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP);
  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_component_minimum_not_less_than_maximum(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  static const dt_remote_quantity_component_descriptor_t equal_bounds[2] = {
    { "component_a", NULL, 5.0, 5.0 }, // minimum == maximum
    { "component_b", "kelvin", 0.0, 25000.0 },
  };
  descriptor->components = equal_bounds;
  assert_quantity_registry_rejects(adapter, so);

  static const dt_remote_quantity_component_descriptor_t inverted_bounds[2] = {
    { "component_a", NULL, 5.0, -5.0 }, // minimum > maximum
    { "component_b", "kelvin", 0.0, 25000.0 },
  };
  descriptor->components = inverted_bounds;
  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_duplicate_descriptor_names(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptors = g_new(dt_remote_quantity_descriptor_t, 2);
  descriptors[0] = make_test_descriptor();
  descriptors[1] = make_test_descriptor(); // same .name deliberately

  dt_remote_quantity_module_adapter_t *adapter = g_new0(dt_remote_quantity_module_adapter_t, 1);
  *adapter = (dt_remote_quantity_module_adapter_t){
    .operation = "temperature",
    .minimum_params_version = 4,
    .maximum_params_version = 4,
    .quantities = descriptors,
    .quantity_count = 2,
    .native_fields = s_test_native_fields,
    .native_field_count = 4,
  };
  g_ptr_array_add(s_registry_test_allocations, descriptors);
  g_ptr_array_add(s_registry_test_allocations, adapter);

  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_collision_with_curve(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  dt_remote_curve_registry_set_lookup_override(colliding_curve_lookup_override);
  assert_quantity_registry_rejects(adapter, so);
  dt_remote_curve_registry_set_lookup_override(NULL);
}

static void test_registry_validate_rejects_collision_with_vector(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  dt_remote_vector_registry_set_lookup_override(colliding_vector_lookup_override);
  assert_quantity_registry_rejects(adapter, so);
  dt_remote_vector_registry_set_lookup_override(NULL);
}

static void test_registry_validate_rejects_collision_with_band(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  dt_remote_band_registry_set_lookup_override(colliding_band_lookup_override);
  assert_quantity_registry_rejects(adapter, so);
  dt_remote_band_registry_set_lookup_override(NULL);
}

static void test_registry_validate_rejects_unresolvable_native_field(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  static const char *const bogus_native_fields[1] = { "does_not_exist" };
  adapter->native_fields = bogus_native_fields;
  adapter->native_field_count = 1;
  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_denylisted_or_wrong_type_native_field(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  // "preset" is both denylisted (remote_edit.c's DENY("preset") row for
  // "temperature") and an int, not a float -- either reason alone is
  // sufficient for rejection.
  static const char *const preset_native_fields[1] = { "preset" };
  adapter->native_fields = preset_native_fields;
  adapter->native_field_count = 1;
  assert_quantity_registry_rejects(adapter, so);
}

static void test_registry_validate_rejects_missing_hooks_without_override(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("exposure");
  assert_non_null(so);
  assert_null(so->remote_quantity_read); // Task 2 presence check, restated

  static const dt_remote_quantity_component_descriptor_t single_component[1] = {
    { "component_a", NULL, 0.0, 1.0 },
  };
  dt_remote_quantity_descriptor_t descriptor = { 0 };
  descriptor.name = "test.exposure_quantity";
  descriptor.display_name = "Test";
  descriptor.components = single_component;
  descriptor.component_count = 1;

  dt_remote_quantity_module_adapter_t adapter = {
    .operation = "exposure",
    .minimum_params_version = 7,
    .maximum_params_version = 7,
    .quantities = &descriptor,
    .quantity_count = 1,
    .native_fields = NULL,
    .native_field_count = 0,
  };

  assert_quantity_registry_rejects(&adapter, so);
}

static void test_registry_validate_accepts_valid_adapter(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  // No hooks override installed: registry_validate only checks hook
  // *presence*, which temperature's real .so satisfies unconditionally
  // (Task 2) -- it never calls the hooks.
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_registry_validate(adapter, so, &error));
  assert_null(error);
}

static void test_registry_validate_null_arguments_fail(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  dt_remote_quantity_module_adapter_t *adapter = new_temperature_test_adapter(&descriptor);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_registry_validate(NULL, so, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_registry_validate(adapter, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_list_schema                                          */
/* ---------------------------------------------------------------------- */

static void test_list_schema_converts_descriptor_in_registry_order(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));

  GPtrArray *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_list_schema(so, &out, &error));
  assert_null(error);
  assert_non_null(out);
  assert_int_equal(out->len, 1);

  const dt_remote_quantity_schema_t *schema = g_ptr_array_index(out, 0);
  assert_string_equal(schema->name, "test.quantity_pair");
  assert_string_equal(schema->display_name, "Test Pair");
  assert_true(schema->derived);
  assert_int_equal(schema->writability, DT_REMOTE_WRITABLE_NOW);
  assert_null(schema->active_when);
  assert_null(schema->writable_when);
  assert_non_null(schema->components);
  assert_int_equal(schema->components->len, 2);

  const dt_remote_quantity_schema_component_t *a = g_ptr_array_index(schema->components, 0);
  assert_string_equal(a->name, "component_a");
  assert_null(a->unit);
  assert_true(a->minimum == QUANTITY_TEST_A_MIN);
  assert_true(a->maximum == QUANTITY_TEST_A_MAX);

  const dt_remote_quantity_schema_component_t *b = g_ptr_array_index(schema->components, 1);
  assert_string_equal(b->name, "component_b");
  assert_string_equal(b->unit, "kelvin");
  assert_true(b->minimum == QUANTITY_TEST_B_MIN);
  assert_true(b->maximum == QUANTITY_TEST_B_MAX);

  g_ptr_array_unref(out);
}

static void test_list_schema_no_adapter_yields_null_and_succeeds(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");

  GPtrArray *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_list_schema(so, &out, &error));
  assert_null(error);
  assert_null(out);
}

static void test_list_schema_null_arguments_fail(void **state)
{
  (void)state;
  dt_iop_module_so_t *so = dt_iop_get_module_so("temperature");

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_list_schema(NULL, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);

  GPtrArray *out = NULL;
  error = NULL;
  assert_false(dt_remote_quantity_list_schema(so, NULL, &error));
  assert_non_null(error);
  dt_remote_error_free(error);
  (void)out;
}

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_read_values                                          */
/* ---------------------------------------------------------------------- */

static void test_read_values_calls_hook_and_stamps_unconditional_active(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);

  poke_temperature_float(fixture->module, "red", fixture->module->params, 5.0f);
  poke_temperature_float(fixture->module, "green", fixture->module->params, 60.0f);

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_null(error);
  assert_non_null(out);

  const dt_remote_quantity_value_t *value = g_hash_table_lookup(out, "test.quantity_pair");
  assert_non_null(value);
  assert_int_equal(value->values->len, 2);
  assert_true(value->active);
  assert_true(value->effective);
  assert_true(value->writable_now);

  const dt_remote_quantity_component_value_t *a = g_ptr_array_index(value->values, 0);
  assert_string_equal(a->name, "component_a");
  assert_true(a->value == 10.0);
  const dt_remote_quantity_component_value_t *b = g_ptr_array_index(value->values, 1);
  assert_string_equal(b->name, "component_b");
  assert_true(b->value == 6000.0);

  g_hash_table_unref(out);
  quantity_fixture_free(fixture);
}

static void test_read_values_no_adapter_returns_empty_table(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_null(error);
  assert_non_null(out);
  assert_int_equal(g_hash_table_size(out), 0);

  g_hash_table_unref(out);
  quantity_fixture_free(fixture);
}

static void test_read_values_null_arguments_fail(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_read_values(NULL, fixture->module->params, &out, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(out);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_read_values(fixture->module, NULL, &out, &error));
  assert_non_null(error);
  assert_null(out);
  dt_remote_error_free(error);

  quantity_fixture_free(fixture);
}

static void test_read_values_hook_failure_fails_closed(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(always_false_quantity_read_hook, always_false_quantity_write_hook);

  GHashTable *out = NULL;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_read_values(fixture->module, fixture->module->params, &out, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_null(out);

  dt_remote_error_free(error);
  quantity_fixture_free(fixture);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_quantity_apply_entries / dt_remote_quantity_apply_patch        */
/* ---------------------------------------------------------------------- */

static dt_remote_semantic_patch_t *make_quantity_entry(const char *name, const char *const *component_names,
                                                       const double *component_values, guint n)
{
  dt_remote_semantic_patch_t *entry = g_new0(dt_remote_semantic_patch_t, 1);
  entry->class_id = DT_REMOTE_PARAMETER_QUANTITY;
  entry->value.quantity.name = g_strdup(name);
  entry->value.quantity.values = make_component_values(component_names, component_values, n);
  return entry;
}

static dt_remote_patch_entry_t *make_scalar_entry(const char *name, double value)
{
  dt_remote_patch_entry_t *entry = g_new0(dt_remote_patch_entry_t, 1);
  entry->name = g_strdup(name);
  entry->value.type = DT_REMOTE_VALUE_FLOAT;
  entry->value.v.f = value;
  return entry;
}

static void test_apply_entries_unknown_id_with_adapter(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 2.0 };
  dt_remote_semantic_patch_t *entry =
    make_quantity_entry("test.nonexistent", s_test_component_names, values, 2);
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, entry);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_patch_unknown_id_no_adapter_at_all(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 2.0 };
  dt_remote_semantic_patch_t *entry =
    make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2);
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(patch.semantic_values, entry);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_patch(fixture->module, fixture->module->params, scratch,
                                              &patch, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_UNKNOWN_FIELD);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(patch.semantic_values);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_duplicate_id_rejected(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 2.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_domain_violation_leaves_scratch_untouched(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 25000.00001 }; // component_b out of domain
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"domain\""));
  assert_non_null(strstr(error->details_json, "\"component\":\"component_b\""));
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_native_conflict_rejected_blob_untouched(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 5500.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_patch_entry_free);
  g_ptr_array_add(patch.scalar_values, make_scalar_entry("red", 3.0)); // "red" is a native field

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, &patch, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INVALID_VALUE);
  assert_non_null(strstr(error->details_json, "\"native_conflict\""));
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_ptr_array_unref(patch.scalar_values);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_no_conflict_when_patch_null(void **state)
{
  (void)state;
  // `patch` may be NULL in pure-engine tests -- the native-conflict check
  // must not crash and must simply not fire.
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 1.0, 5500.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                               entries, NULL, &error));
  assert_null(error);

  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_successful_write_preserves_other_bytes(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);
  void *expected = scratch_params_new(fixture->module);

  static const double values[2] = { 10.0, 6000.0 }; // -> red=5.0, green=60.0
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                               entries, NULL, &error));
  assert_null(error);

  // Build the expected buffer by poking only red/green into a copy of the
  // pre-call snapshot -- exactly the narrowing the fake write hook performs
  // -- then a full params_size memcmp proves every other byte (blue,
  // various, preset, and any padding) is untouched.
  poke_temperature_float(fixture->module, "red", expected, 5.0f);
  poke_temperature_float(fixture->module, "green", expected, 60.0f);
  assert_memory_equal(scratch, expected, fixture->module->params_size);

  g_ptr_array_unref(entries);
  g_free(scratch);
  g_free(expected);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_stray_byte_write_hook_rolls_back(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, stray_byte_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 10.0, 6000.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_scripted_hook_failure_rolls_back(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(always_false_quantity_read_hook, always_false_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 10.0, 6000.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_no_override_real_hooks_fail_closed(void **state)
{
  (void)state;
  // No hooks override installed: the real temperature.so hooks run, and
  // fail closed because the unit-test fixture never builds `gui_data`
  // (see this file's own top comment) -- proving the engine surfaces that
  // failure as DT_REMOTE_ERR_INTERNAL rather than crashing or silently
  // succeeding.
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  void *scratch = scratch_params_new(fixture->module);

  static const double values[2] = { 10.0, 6000.0 };
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(entries, make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch,
                                                entries, NULL, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  dt_remote_error_free(error);
  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_entries_null_arguments_fail(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  void *scratch = scratch_params_new(fixture->module);
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_entries(NULL, fixture->module->params, scratch, entries, NULL,
                                                &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, NULL, scratch, entries, NULL, &error));
  assert_non_null(error);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, NULL, entries,
                                                NULL, &error));
  assert_non_null(error);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_apply_entries(fixture->module, fixture->module->params, scratch, NULL,
                                                NULL, &error));
  assert_non_null(error);
  dt_remote_error_free(error);

  g_ptr_array_unref(entries);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_patch_null_arguments_fail(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  void *scratch = scratch_params_new(fixture->module);
  dt_remote_patch_t patch = { 0 };

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_quantity_apply_patch(NULL, fixture->module->params, scratch, &patch, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  dt_remote_error_free(error);

  error = NULL;
  assert_false(dt_remote_quantity_apply_patch(fixture->module, fixture->module->params, scratch, NULL,
                                              &error));
  assert_non_null(error);
  dt_remote_error_free(error);

  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_patch_empty_semantic_values_is_trivial_success(void **state)
{
  (void)state;
  // No adapter installed at all -- the production table is empty. A patch
  // with zero quantity entries must never even look up the registry.
  quantity_fixture_t *fixture = temperature_fixture_new();
  void *scratch = scratch_params_new(fixture->module);
  dt_remote_patch_t patch = { 0 };

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                             &error));
  assert_null(error);
  assert_memory_equal(scratch, fixture->module->params, fixture->module->params_size);

  g_free(scratch);
  quantity_fixture_free(fixture);
}

static void test_apply_patch_mixed_class_entries_skip_non_quantity(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);
  void *expected = scratch_params_new(fixture->module);

  static const double values[2] = { 10.0, 6000.0 };
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);

  // A foreign-class entry: never touched by this engine, must not crash
  // when its (unrelated) union arm is never read.
  dt_remote_semantic_patch_t *foreign = g_new0(dt_remote_semantic_patch_t, 1);
  foreign->class_id = DT_REMOTE_PARAMETER_VECTOR;
  foreign->value.vector.name = g_strdup("vector.unrelated");
  foreign->value.vector.values = g_array_sized_new(FALSE, FALSE, sizeof(double), 0);
  g_ptr_array_add(patch.semantic_values, foreign);

  g_ptr_array_add(patch.semantic_values,
                  make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                             &error));
  assert_null(error);

  poke_temperature_float(fixture->module, "red", expected, 5.0f);
  poke_temperature_float(fixture->module, "green", expected, 60.0f);
  assert_memory_equal(scratch, expected, fixture->module->params_size);

  g_ptr_array_unref(patch.semantic_values);
  g_free(scratch);
  g_free(expected);
  quantity_fixture_free(fixture);
}

static void test_apply_patch_then_read_values_round_trips(void **state)
{
  (void)state;
  quantity_fixture_t *fixture = temperature_fixture_new();
  dt_remote_quantity_descriptor_t *descriptor = NULL;
  install_quantity_adapter(new_temperature_test_adapter(&descriptor));
  dt_remote_quantity_set_hooks_override(fake_quantity_read_hook, fake_quantity_write_hook);
  void *scratch = scratch_params_new(fixture->module);

  // Both values are chosen so the fake hooks' /2 and /100 narrowing to
  // float is exact (halves and whole numbers), so the round-trip compares
  // equal exactly rather than needing a tolerance.
  static const double values[2] = { -12.5, 4500.0 };
  dt_remote_patch_t patch = { 0 };
  patch.semantic_values = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_semantic_patch_free);
  g_ptr_array_add(patch.semantic_values,
                  make_quantity_entry("test.quantity_pair", s_test_component_names, values, 2));

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_quantity_apply_patch(fixture->module, fixture->module->params, scratch, &patch,
                                             &error));
  assert_null(error);

  GHashTable *out = NULL;
  assert_true(dt_remote_quantity_read_values(fixture->module, scratch, &out, &error));
  assert_null(error);
  const dt_remote_quantity_value_t *value = g_hash_table_lookup(out, "test.quantity_pair");
  assert_non_null(value);
  const dt_remote_quantity_component_value_t *a = g_ptr_array_index(value->values, 0);
  const dt_remote_quantity_component_value_t *b = g_ptr_array_index(value->values, 1);
  assert_true(a->value == values[0]);
  assert_true(b->value == values[1]);

  g_hash_table_unref(out);
  g_ptr_array_unref(patch.semantic_values);
  g_free(scratch);
  quantity_fixture_free(fixture);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_schema_free_fully_populated),
    cmocka_unit_test(test_schema_free_minimal),
    cmocka_unit_test(test_schema_free_null_is_safe),
    cmocka_unit_test(test_value_free_fully_populated),
    cmocka_unit_test(test_value_free_null_is_safe),

    cmocka_unit_test(test_temperature_so_exports_both_hooks),
    cmocka_unit_test(test_exposure_so_exports_neither_hook),

    cmocka_unit_test(test_validate_accepts_well_formed_values),
    cmocka_unit_test(test_validate_rejects_missing_component),
    cmocka_unit_test(test_validate_rejects_unknown_component),
    cmocka_unit_test(test_validate_rejects_duplicate_component),
    cmocka_unit_test(test_validate_rejects_non_finite),
    cmocka_unit_test(test_validate_rejects_domain_just_outside_double_boundary),
    cmocka_unit_test(test_validate_accepts_domain_exactly_at_boundary),
    cmocka_unit_test(test_validate_null_descriptor_fails_internal),

    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_invalid_adapter_envelope,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_component_count_zero,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_component_count_exceeds_cap,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_component_minimum_not_less_than_maximum,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_duplicate_descriptor_names,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_collision_with_curve,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_collision_with_vector,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_collision_with_band,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_unresolvable_native_field,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_denylisted_or_wrong_type_native_field,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_rejects_missing_hooks_without_override,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_accepts_valid_adapter,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_registry_validate_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_list_schema_converts_descriptor_in_registry_order,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_no_adapter_yields_null_and_succeeds,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_list_schema_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_read_values_calls_hook_and_stamps_unconditional_active,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_no_adapter_returns_empty_table,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_read_values_hook_failure_fails_closed,
                                    lookup_override_test_setup, lookup_override_test_teardown),

    cmocka_unit_test_setup_teardown(test_apply_entries_unknown_id_with_adapter,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_unknown_id_no_adapter_at_all,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_duplicate_id_rejected,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_domain_violation_leaves_scratch_untouched,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_native_conflict_rejected_blob_untouched,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_no_conflict_when_patch_null,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_successful_write_preserves_other_bytes,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_stray_byte_write_hook_rolls_back,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_scripted_hook_failure_rolls_back,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_no_override_real_hooks_fail_closed,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_entries_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_null_arguments_fail,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_empty_semantic_values_is_trivial_success,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_mixed_class_entries_skip_non_quantity,
                                    lookup_override_test_setup, lookup_override_test_teardown),
    cmocka_unit_test_setup_teardown(test_apply_patch_then_read_values_round_trips,
                                    lookup_override_test_setup, lookup_override_test_teardown),
  };

  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
