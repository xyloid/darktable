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
#include "develop/blend.h"

#include <glib.h>

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
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
