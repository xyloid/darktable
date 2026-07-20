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
 * cmocka unit tests for the Tier-1 blend surface:
 *  - Task 1: GUI-parity of dt_develop_blend_mode_sections() against the
 *    pre-refactor hardcoded combobox population (frozen here as data).
 *  - Tasks 2-4 append their sections to this file.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "control/remote_blend.h"
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "develop/imageop.h"

#include <glib.h>
#include <math.h>

// Flattens dt_develop_blend_mode_sections(csp) exactly the way
// dt_bauhaus_combobox_add_introspection() walks dt_develop_blend_mode_names:
// per section row, scan tuples to value==from, append until value==to.
static GArray *_flatten_sections(dt_develop_blend_colorspace_t csp)
{
  GArray *out = g_array_new(FALSE, FALSE, sizeof(int));
  for(const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(csp);
      s && s->section; s++)
  {
    const dt_introspection_type_enum_tuple_t *item = dt_develop_blend_mode_names;
    while(item->name && item->value != (int)s->from) item++;
    for(; item->name; item++)
    {
      g_array_append_val(out, item->value);
      if(item->value == (int)s->to) break;
    }
  }
  return out;
}

static void _assert_flattened_equals(dt_develop_blend_colorspace_t csp,
                                     const int *expected, guint count)
{
  GArray *got = _flatten_sections(csp);
  assert_int_equal(got->len, count);
  for(guint i = 0; i < count; i++)
    assert_int_equal(g_array_index(got, int, i), expected[i]);
  g_array_unref(got);
}

// Frozen from blend_gui.c's pre-refactor population (this plan's Task 1),
// expanded through dt_develop_blend_mode_names[] tuple order.
static const int EXPECTED_RAW[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
};

static const int EXPECTED_LAB[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
  DEVELOP_BLEND_LAB_LIGHTNESS, DEVELOP_BLEND_LAB_A, DEVELOP_BLEND_LAB_B,
  DEVELOP_BLEND_LAB_COLOR,
  DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLOR, DEVELOP_BLEND_COLORADJUST,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static const int EXPECTED_RGB_DISPLAY[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_BOUNDED,
  DEVELOP_BLEND_LIGHTEN, DEVELOP_BLEND_ADD, DEVELOP_BLEND_SCREEN,
  DEVELOP_BLEND_DARKEN, DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_MULTIPLY,
  DEVELOP_BLEND_OVERLAY, DEVELOP_BLEND_SOFTLIGHT, DEVELOP_BLEND_HARDLIGHT,
  DEVELOP_BLEND_VIVIDLIGHT, DEVELOP_BLEND_LINEARLIGHT, DEVELOP_BLEND_PINLIGHT,
  DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_G, DEVELOP_BLEND_RGB_B,
  DEVELOP_BLEND_HSV_VALUE, DEVELOP_BLEND_HSV_COLOR,
  DEVELOP_BLEND_HUE, DEVELOP_BLEND_COLOR, DEVELOP_BLEND_COLORADJUST,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static const int EXPECTED_RGB_SCENE[] = {
  DEVELOP_BLEND_NORMAL2, DEVELOP_BLEND_AVERAGE, DEVELOP_BLEND_DIFFERENCE2,
  DEVELOP_BLEND_MULTIPLY, DEVELOP_BLEND_DIVIDE, DEVELOP_BLEND_ADD,
  DEVELOP_BLEND_SUBTRACT, DEVELOP_BLEND_GEOMETRIC_MEAN, DEVELOP_BLEND_HARMONIC_MEAN,
  DEVELOP_BLEND_RGB_R, DEVELOP_BLEND_RGB_G, DEVELOP_BLEND_RGB_B,
  DEVELOP_BLEND_LIGHTNESS, DEVELOP_BLEND_CHROMATICITY,
};

static void test_mode_sections_parity_raw(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RAW, EXPECTED_RAW, G_N_ELEMENTS(EXPECTED_RAW)); }
static void test_mode_sections_parity_lab(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_LAB, EXPECTED_LAB, G_N_ELEMENTS(EXPECTED_LAB)); }
static void test_mode_sections_parity_rgb_display(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RGB_DISPLAY, EXPECTED_RGB_DISPLAY, G_N_ELEMENTS(EXPECTED_RGB_DISPLAY)); }
static void test_mode_sections_parity_rgb_scene(void **state)
{ (void)state; _assert_flattened_equals(DEVELOP_BLEND_CS_RGB_SCENE, EXPECTED_RGB_SCENE, G_N_ELEMENTS(EXPECTED_RGB_SCENE)); }
static void test_mode_sections_none_is_empty(void **state)
{
  (void)state;
  const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(DEVELOP_BLEND_CS_NONE);
  assert_non_null(s);
  assert_null(s->section);
}
// No mode listed for any colorspace is deprecated/obsolete: assert none of
// the flattened values appears past the "deprecated" marker in
// dt_develop_blend_mode_names (DIFFERENCE, SUBTRACT_INVERSE, DIVIDE_INVERSE,
// LAB_L) nor is an *_OBSOLETE enumerator.
static void test_mode_sections_never_list_deprecated(void **state)
{
  (void)state;
  const int deprecated[] = { DEVELOP_BLEND_DIFFERENCE, DEVELOP_BLEND_SUBTRACT_INVERSE,
                             DEVELOP_BLEND_DIVIDE_INVERSE, DEVELOP_BLEND_LAB_L };
  const dt_develop_blend_colorspace_t spaces[] = { DEVELOP_BLEND_CS_RAW, DEVELOP_BLEND_CS_LAB,
    DEVELOP_BLEND_CS_RGB_DISPLAY, DEVELOP_BLEND_CS_RGB_SCENE };
  for(guint c = 0; c < G_N_ELEMENTS(spaces); c++)
  {
    GArray *got = _flatten_sections(spaces[c]);
    for(guint i = 0; i < got->len; i++)
      for(guint d = 0; d < G_N_ELEMENTS(deprecated); d++)
        assert_int_not_equal(g_array_index(got, int, i), deprecated[d]);
    g_array_unref(got);
  }
}

/* ------------------------------------------------------------------ */
/* Task 2: mask_mode mapping + mode expansion                          */
/* ------------------------------------------------------------------ */

static void test_mask_mode_string_mapping(void **state)
{
  (void)state;
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_DISABLED), "off");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED), "uniform");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL),
                      "parametric");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK),
                      "drawn");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL),
                      "drawn+parametric");
  assert_string_equal(dt_remote_blend_mask_mode_string(DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER),
                      "raster");
  // raster wins over any other bit combination
  assert_string_equal(dt_remote_blend_mask_mode_string(
                        DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK | DEVELOP_MASK_RASTER),
                      "raster");
}

static void test_mask_mode_from_string(void **state)
{
  (void)state;
  uint32_t v = 999;
  assert_true(dt_remote_blend_mask_mode_from_string("off", &v));
  assert_int_equal(v, DEVELOP_MASK_DISABLED);
  assert_true(dt_remote_blend_mask_mode_from_string("uniform", &v));
  assert_int_equal(v, DEVELOP_MASK_ENABLED);
  // read-only compounds and junk are rejected
  assert_false(dt_remote_blend_mask_mode_from_string("drawn", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("parametric", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("raster", &v));
  assert_false(dt_remote_blend_mask_mode_from_string("", &v));
  assert_false(dt_remote_blend_mask_mode_from_string(NULL, &v));
}

static void test_mode_names_for_colorspace_matches_sections(void **state)
{
  (void)state;
  // spot-check RGB scene: first name, arithmetic run, count matches the
  // Task-1 frozen expectation (14 modes)
  GPtrArray *names = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_RGB_SCENE);
  assert_int_equal(names->len, 14);
  assert_string_equal(g_ptr_array_index(names, 0), "DEVELOP_BLEND_NORMAL2");
  assert_string_equal(g_ptr_array_index(names, 3), "DEVELOP_BLEND_MULTIPLY");
  assert_string_equal(g_ptr_array_index(names, 8), "DEVELOP_BLEND_HARMONIC_MEAN");
  assert_string_equal(g_ptr_array_index(names, 13), "DEVELOP_BLEND_CHROMATICITY");
  g_ptr_array_unref(names);

  GPtrArray *lab = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_LAB);
  assert_int_equal(lab->len, 25);
  assert_string_equal(g_ptr_array_index(lab, 16), "DEVELOP_BLEND_LAB_LIGHTNESS");
  g_ptr_array_unref(lab);

  GPtrArray *none = dt_remote_blend_mode_names_for_colorspace(DEVELOP_BLEND_CS_NONE);
  assert_int_equal(none->len, 0);
  g_ptr_array_unref(none);
}

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_blend_confdir = NULL;

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_blend_confdir = g_dir_make_tmp("test_remote_blend-XXXXXX", &gerror);
  if(!s_blend_confdir)
  {
    fprintf(stderr, "test_remote_blend: scratch confdir failed: %s\n", gerror->message);
    g_error_free(gerror);
    return -1;
  }
  char *argv_override[] = {
    "test_remote_blend",
    "--configdir", s_blend_confdir,
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
  if(s_blend_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_blend_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_blend: could not remove %s\n", s_blend_confdir);
    g_free(cmd);
    g_clear_pointer(&s_blend_confdir, g_free);
  }
  return 0;
}

typedef struct blend_fixture_t
{
  dt_develop_t dev;
  dt_iop_module_t *module;
} blend_fixture_t;

static blend_fixture_t *blend_fixture_new(const char *op)
{
  blend_fixture_t *fixture = g_new0(blend_fixture_t, 1);
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

static void blend_fixture_free(blend_fixture_t *fixture)
{
  if(!fixture) return;
  dt_dev_cleanup(&fixture->dev);
  g_free(fixture);
}

static JsonObject *_node_object(JsonNode *node)
{
  assert_non_null(node);
  assert_true(JSON_NODE_HOLDS_OBJECT(node));
  return json_node_get_object(node);
}

// "exposure" supports blending (RGB module); "rawprepare" does not.
static void test_schema_null_for_non_blending_module(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("rawprepare");
  assert_null(dt_remote_blend_schema(fx->module));
  assert_null(dt_remote_blend_read(fx->module));
  blend_fixture_free(fx);
}

static void test_schema_shape_for_rgb_module(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);

  JsonObject *mm = json_object_get_object_member(schema, "mask_mode");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_false(json_object_get_boolean_member(mm, "current_extra_bits"));
  JsonArray *mm_values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(mm_values), 2);
  assert_string_equal(json_array_get_string_element(mm_values, 0), "off");
  assert_string_equal(json_array_get_string_element(mm_values, 1), "uniform");

  JsonObject *cs = json_object_get_object_member(schema, "colorspace");
  assert_true(json_object_get_boolean_member(cs, "writable"));
  JsonArray *cs_values = json_object_get_array_member(cs, "values");
  assert_int_equal(json_array_get_length(cs_values), 2);
  assert_string_equal(json_array_get_string_element(cs_values, 0), "DEVELOP_BLEND_CS_RGB_DISPLAY");
  assert_string_equal(json_array_get_string_element(cs_values, 1), "DEVELOP_BLEND_CS_RGB_SCENE");
  const char *cs_default = json_object_get_string_member(cs, "default");
  assert_true(!strcmp(cs_default, "DEVELOP_BLEND_CS_RGB_DISPLAY")
              || !strcmp(cs_default, "DEVELOP_BLEND_CS_RGB_SCENE"));

  JsonObject *mode = json_object_get_object_member(schema, "mode");
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(fx->module, fx->module->blend_params->blend_cst);
  GPtrArray *expected = dt_remote_blend_mode_names_for_colorspace(eff);
  JsonArray *mode_values = json_object_get_array_member(mode, "values");
  assert_int_equal(json_array_get_length(mode_values), expected->len);
  for(guint i = 0; i < expected->len; i++)
    assert_string_equal(json_array_get_string_element(mode_values, i),
                        g_ptr_array_index(expected, i));
  g_ptr_array_unref(expected);

  JsonObject *fulcrum = json_object_get_object_member(schema, "fulcrum");
  JsonArray *range = json_object_get_array_member(fulcrum, "range");
  assert_int_equal((int)json_array_get_double_element(range, 0), -18);
  assert_int_equal((int)json_array_get_double_element(range, 1), 18);
  JsonArray *soft = json_object_get_array_member(fulcrum, "soft_range");
  assert_int_equal((int)json_array_get_double_element(soft, 0), -3);
  assert_string_equal(json_object_get_string_member(fulcrum, "unit"), "EV");

  JsonObject *details = json_object_get_object_member(schema, "details");
  assert_false(json_object_get_boolean_member(details, "writable"));

  JsonObject *fg = json_object_get_object_member(schema, "feathering_guide");
  assert_int_equal(json_array_get_length(json_object_get_array_member(fg, "values")), 4);

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_schema_mask_mode_not_writable_with_extra_bits(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_false(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_defaults(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mask_mode"), "off");
  assert_string_equal(json_object_get_string_member(o, "colorspace"), "DEVELOP_BLEND_CS_NONE");
  const char *eff = json_object_get_string_member(o, "effective_colorspace");
  assert_true(!strcmp(eff, "DEVELOP_BLEND_CS_RGB_DISPLAY") || !strcmp(eff, "DEVELOP_BLEND_CS_RGB_SCENE"));
  assert_string_equal(json_object_get_string_member(o, "mode"), "DEVELOP_BLEND_NORMAL2");
  assert_false(json_object_get_boolean_member(o, "reverse"));
  assert_float_equal(json_object_get_double_member(o, "opacity"), 100.0, 1e-6);
  assert_float_equal(json_object_get_double_member(o, "fulcrum"), 0.0, 1e-6);
  assert_string_equal(json_object_get_string_member(o, "feathering_guide"),
                      "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR");
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_reverse_and_deprecated_mode(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_LAB_L | DEVELOP_BLEND_REVERSE;
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mode"), "DEVELOP_BLEND_LAB_L");
  assert_true(json_object_get_boolean_member(o, "reverse"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_non_finite_serializes_null(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->opacity = NAN;
  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_true(json_object_get_null_member(o, "opacity"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_mode_sections_parity_raw),
    cmocka_unit_test(test_mode_sections_parity_lab),
    cmocka_unit_test(test_mode_sections_parity_rgb_display),
    cmocka_unit_test(test_mode_sections_parity_rgb_scene),
    cmocka_unit_test(test_mode_sections_none_is_empty),
    cmocka_unit_test(test_mode_sections_never_list_deprecated),
    cmocka_unit_test(test_mask_mode_string_mapping),
    cmocka_unit_test(test_mask_mode_from_string),
    cmocka_unit_test(test_mode_names_for_colorspace_matches_sections),
    cmocka_unit_test(test_schema_null_for_non_blending_module),
    cmocka_unit_test(test_schema_shape_for_rgb_module),
    cmocka_unit_test(test_schema_mask_mode_not_writable_with_extra_bits),
    cmocka_unit_test(test_read_defaults),
    cmocka_unit_test(test_read_reverse_and_deprecated_mode),
    cmocka_unit_test(test_read_non_finite_serializes_null),
  };
  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
