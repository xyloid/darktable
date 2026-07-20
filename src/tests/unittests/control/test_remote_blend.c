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
 * cmocka unit tests for the Tier-1 blend base plus Tier-2 extensions:
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

// The GUI blendif channel tables are non-static `const` (blend_gui.c) and
// directly linkable; extern them for the GUI-parity binding below.
extern const dt_iop_gui_blendif_channel_t Lab_channels[];
extern const dt_iop_gui_blendif_channel_t rgb_channels[];
extern const dt_iop_gui_blendif_channel_t rgbj_channels[];

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

/* ------------------------------------------------------------------ */
/* Task 1: blendif channel table + pure helpers (Tier 2 / M-B)         */
/* ------------------------------------------------------------------ */

static const dt_remote_blendif_channel_t *_find_channel(dt_develop_blend_colorspace_t csp,
                                                        const char *name)
{
  for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(csp); c && c->name; c++)
    if(!strcmp(c->name, name)) return c;
  return NULL;
}

// Bind the engine table to the externed GUI table entry-for-entry: same
// slots, same boost enablement, same boost storage offset. Drift in the
// GUI tables becomes a failure here with zero GUI changes (design decision 6).
static void _assert_parity(dt_develop_blend_colorspace_t csp,
                           const dt_iop_gui_blendif_channel_t *gui,
                           const char *const *wire_names)
{
  const dt_remote_blendif_channel_t *eng = dt_remote_blendif_channels(csp);
  assert_non_null(eng);
  guint i = 0;
  for(; gui[i].label; i++)   // GUI tables terminate with a {NULL} entry
  {
    assert_non_null(eng[i].name);
    assert_string_equal(eng[i].name, wire_names[i]);
    assert_int_equal(eng[i].slot_in, gui[i].param_channels[0]);
    assert_int_equal(eng[i].slot_out, gui[i].param_channels[1]);
    assert_int_equal(eng[i].boost_supported, gui[i].boost_factor_enabled);
    assert_float_equal(eng[i].boost_offset, gui[i].boost_factor_offset, 1e-6);
  }
  // engine table ends exactly where the GUI table ends
  assert_null(eng[i].name);
}

static void test_channels_parity_lab(void **state)
{
  (void)state;
  static const char *const names[] = { "L", "a", "b", "C", "h" };
  _assert_parity(DEVELOP_BLEND_CS_LAB, Lab_channels, names);
}
static void test_channels_parity_rgb_display(void **state)
{
  (void)state;
  static const char *const names[] = { "g", "R", "G", "B", "H", "S", "l" };
  _assert_parity(DEVELOP_BLEND_CS_RGB_DISPLAY, rgb_channels, names);
}
static void test_channels_parity_rgb_scene(void **state)
{
  (void)state;
  static const char *const names[] = { "g", "R", "G", "B", "Jz", "Cz", "hz" };
  _assert_parity(DEVELOP_BLEND_CS_RGB_SCENE, rgbj_channels, names);
}
static void test_channels_none_and_raw_unsupported(void **state)
{
  (void)state;
  assert_null(dt_remote_blendif_channels(DEVELOP_BLEND_CS_NONE));
  assert_null(dt_remote_blendif_channels(DEVELOP_BLEND_CS_RAW));
}

static void test_boost_offsets_and_ranges(void **state)
{
  (void)state;
  // Jz/Cz carry the -6.64385619 storage offset; everything else 0.
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "Jz")->boost_offset,
                     -6.64385619f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "Cz")->boost_offset,
                     -6.64385619f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->boost_offset, 0.0f, 1e-6);
  // hue channels have no boost; g/R/G/B/L do.
  assert_false(_find_channel(DEVELOP_BLEND_CS_LAB, "h")->boost_supported);
  assert_true(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->boost_supported);
  assert_false(_find_channel(DEVELOP_BLEND_CS_RGB_SCENE, "hz")->boost_supported);
  // Lab a/b marker offset is 0.5; others 0.
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "a")->marker_offset, 0.5f, 1e-6);
  assert_float_equal(_find_channel(DEVELOP_BLEND_CS_LAB, "L")->marker_offset, 0.0f, 1e-6);

  // Bind each display-hint category to the GUI print functions: normalized
  // channels are percentages, a/b are centered 256-scale values, and hues
  // are degrees. Empty string means deliberately unitless.
  const dt_remote_blendif_channel_t *lab_a =
    _find_channel(DEVELOP_BLEND_CS_LAB, "a");
  const dt_remote_blendif_channel_t *rgb_g =
    _find_channel(DEVELOP_BLEND_CS_RGB_DISPLAY, "g");
  const dt_remote_blendif_channel_t *rgb_h =
    _find_channel(DEVELOP_BLEND_CS_RGB_DISPLAY, "H");
  assert_float_equal(lab_a->display_factor, 256.0f, 1e-6);
  assert_string_equal(lab_a->display_unit, "");
  assert_float_equal(rgb_g->display_factor, 100.0f, 1e-6);
  assert_string_equal(rgb_g->display_unit, "%");
  assert_float_equal(rgb_h->display_factor, 360.0f, 1e-6);
  assert_string_equal(rgb_h->display_unit, "\xc2\xb0");
}

static void test_slot_enabled_inverted_pack(void **state)
{
  (void)state;
  // Exercise every in/out slot in every supported family, not one sample.
  const dt_develop_blend_colorspace_t families[] = {
    DEVELOP_BLEND_CS_LAB, DEVELOP_BLEND_CS_RGB_DISPLAY, DEVELOP_BLEND_CS_RGB_SCENE
  };
  for(guint f = 0; f < G_N_ELEMENTS(families); f++)
    for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(families[f]);
        c && c->name; c++)
    {
      const int slots[] = { c->slot_in, c->slot_out };
      for(guint i = 0; i < G_N_ELEMENTS(slots); i++)
      {
        const int slot = slots[i];
        uint32_t bf = dt_remote_blendif_slot_pack(0u, slot, TRUE, TRUE);
        assert_true(dt_remote_blendif_slot_enabled(bf, slot));
        assert_true(dt_remote_blendif_slot_inverted(bf, slot));
        assert_int_equal(bf, (1u << slot) | (1u << (16 + slot)));
        bf = dt_remote_blendif_slot_pack(bf, slot, FALSE, FALSE);
        assert_false(dt_remote_blendif_slot_enabled(bf, slot));
        assert_false(dt_remote_blendif_slot_inverted(bf, slot));
        assert_int_equal(bf, 0u);
      }
    }

  // the legacy DEVELOP_BLENDIF_active bit (31) is always stripped by pack
  uint32_t bf = dt_remote_blendif_slot_pack(1u << 31, 0, TRUE, FALSE);
  assert_int_equal(bf & (1u << 31), 0u);
}

static void test_markers_enable_rule(void **state)
{
  (void)state;
  const float full_span[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
  const float half[4] = { 0.0f, 0.0f, 0.55f, 0.65f };
  const float hard[4] = { 0.3f, 0.3f, 0.3f, 0.3f };
  assert_false(dt_remote_blendif_markers_enable(full_span)); // identity -> disabled
  assert_true(dt_remote_blendif_markers_enable(half));
  assert_true(dt_remote_blendif_markers_enable(hard));
}

static void test_mask_mode_targets_and_transition_matrix(void **state)
{
  (void)state;
  typedef struct { const char *name; uint32_t bits; } target_t;
  static const target_t targets[] = {
    { "off", DEVELOP_MASK_DISABLED },
    { "uniform", DEVELOP_MASK_ENABLED },
    { "parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL },
    { "drawn", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK },
    { "drawn+parametric", DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL },
  };
  static const uint32_t rows[] = {
    DEVELOP_MASK_DISABLED,
    DEVELOP_MASK_ENABLED,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL,
  };

  // Every target is exercised through the transition helper below. The
  // public M-A parser remains off/uniform until Task 4 so Tasks 1-3 are
  // independently green and do not broaden shipped patch behavior early.
  uint32_t ignored = 0;

  // Every non-raster cell in the authoritative appendix: success exactly
  // when stored and target MASK ownership agree.
  for(guint r = 0; r < G_N_ELEMENTS(rows); r++)
    for(guint t = 0; t < G_N_ELEMENTS(targets); t++)
    {
      uint32_t projected = UINT32_MAX;
      const char *constraint = NULL;
      const gboolean same_drawn =
        ((rows[r] & DEVELOP_MASK_MASK) != 0)
        == ((targets[t].bits & DEVELOP_MASK_MASK) != 0);
      assert_int_equal(dt_remote_blend_mask_mode_transition(
                         rows[r], targets[t].name, &projected, &constraint),
                       same_drawn);
      if(same_drawn)
      {
        assert_null(constraint);
        assert_int_equal(projected, targets[t].bits);
      }
      else
        assert_string_equal(constraint, "drawn_via_attach_only");
    }

  // The helper owns only ENABLED|CONDITIONAL; a future/unowned bit is
  // preserved exactly.
  const uint32_t future_bit = 1u << 17;
  uint32_t projected = 0;
  const char *constraint = NULL;
  assert_true(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_ENABLED | future_bit, "parametric", &projected, &constraint));
  assert_int_equal(projected,
                   DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL | future_bit);

  assert_false(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER, "off", &ignored, &constraint));
  assert_string_equal(constraint, "raster_unsupported");
  assert_false(dt_remote_blend_mask_mode_transition(
    DEVELOP_MASK_DISABLED, "bogus", &ignored, &constraint));
  assert_string_equal(constraint, "unknown_value");
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
  assert_int_equal(json_array_get_length(mm_values), 3);
  assert_string_equal(json_array_get_string_element(mm_values, 0), "off");
  assert_string_equal(json_array_get_string_element(mm_values, 1), "uniform");
  assert_string_equal(json_array_get_string_element(mm_values, 2), "parametric");

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

static void test_schema_mask_mode_extra_bits_metadata(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);

  fx->module->blend_params->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  node = dt_remote_blend_schema(fx->module);
  mm = json_object_get_object_member(_node_object(node), "mask_mode");
  assert_false(json_object_get_boolean_member(mm, "writable"));
  assert_true(json_object_get_boolean_member(mm, "current_extra_bits"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

/* ------------------------------------------------------------------ */
/* Task 2: parametric + combine schema                                 */
/* ------------------------------------------------------------------ */

static void _assert_channel_vocabulary(JsonObject *channels,
                                       const char *const *names,
                                       guint names_len)
{
  assert_int_equal((guint)json_object_get_size(channels), names_len * 2);
  for(guint i = 0; i < names_len; i++)
  {
    gchar *slot = g_strdup_printf("%s_in", names[i]);
    assert_true(json_object_has_member(channels, slot));
    g_free(slot);
    slot = g_strdup_printf("%s_out", names[i]);
    assert_true(json_object_has_member(channels, slot));
    g_free(slot);
  }
}

static void _assert_shifted_boost_range(JsonObject *channels, const char *slot_name)
{
  JsonObject *slot = json_object_get_object_member(channels, slot_name);
  JsonObject *boost = json_object_get_object_member(slot, "boost");
  JsonArray *range = json_object_get_array_member(boost, "range");
  assert_float_equal(json_array_get_double_element(range, 0), -6.64385619, 1e-5);
  assert_float_equal(json_array_get_double_element(range, 1), 11.35614381, 1e-5);
}

static void test_schema_parametric_for_rgb_scene(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);

  // Non-drawn state: the exact writable vocabulary is off/uniform/parametric.
  JsonObject *mm_schema = json_object_get_object_member(schema, "mask_mode");
  JsonArray *mm = json_object_get_array_member(mm_schema, "values");
  assert_int_equal(json_array_get_length(mm), 3);
  assert_string_equal(json_array_get_string_element(mm, 0), "off");
  assert_string_equal(json_array_get_string_element(mm, 1), "uniform");
  assert_string_equal(json_array_get_string_element(mm, 2), "parametric");
  assert_true(json_object_get_boolean_member(mm_schema, "writable"));

  // combine enum, four values, writable
  JsonObject *combine = json_object_get_object_member(schema, "combine");
  assert_true(json_object_get_boolean_member(combine, "writable"));
  JsonArray *cv = json_object_get_array_member(combine, "values");
  assert_int_equal(json_array_get_length(cv), 4);
  assert_string_equal(json_array_get_string_element(cv, 0), "exclusive");
  assert_string_equal(json_array_get_string_element(cv, 1), "inclusive");
  assert_string_equal(json_array_get_string_element(cv, 2), "exclusive_inverted");
  assert_string_equal(json_array_get_string_element(cv, 3), "inclusive_inverted");

  JsonObject *override = json_object_get_object_member(schema, "allow_inverted_combine");
  assert_string_equal(json_object_get_string_member(override, "type"), "bool");
  assert_true(json_object_get_boolean_member(override, "writable"));
  assert_true(json_object_get_boolean_member(override, "write_only"));

  // RGB scene is forced: validate its exact vocabulary and shifted boosts
  // without a runtime family conditional that could skip this coverage.
  JsonObject *channels = json_object_get_object_member(
    json_object_get_object_member(schema, "parametric"), "channels");
  static const char *const names[] = { "g", "R", "G", "B", "Jz", "Cz", "hz" };
  _assert_channel_vocabulary(channels, names, G_N_ELEMENTS(names));
  _assert_shifted_boost_range(channels, "Jz_in");
  _assert_shifted_boost_range(channels, "Jz_out");
  _assert_shifted_boost_range(channels, "Cz_in");
  _assert_shifted_boost_range(channels, "Cz_out");
  assert_true(json_object_get_null_member(
    json_object_get_object_member(channels, "hz_in"), "boost"));
  assert_true(json_object_get_null_member(
    json_object_get_object_member(channels, "hz_out"), "boost"));

  JsonArray *md = json_object_get_array_member(
    json_object_get_object_member(channels, "g_in"), "markers_domain");
  assert_float_equal(json_array_get_double_element(md, 0), 0.0, 1e-6);
  assert_float_equal(json_array_get_double_element(md, 1), 1.0, 1e-6);

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_schema_parametric_for_rgb_display(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_DISPLAY;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);
  assert_true(json_object_has_member(schema, "parametric"));
  JsonObject *channels = json_object_get_object_member(
    json_object_get_object_member(schema, "parametric"), "channels");

  static const char *const names[] = { "g", "R", "G", "B", "H", "S", "l" };
  _assert_channel_vocabulary(channels, names, G_N_ELEMENTS(names));
  static const char *const non_boost_slots[] = {
    "H_in", "H_out", "S_in", "S_out", "l_in", "l_out"
  };
  for(guint i = 0; i < G_N_ELEMENTS(non_boost_slots); i++)
    assert_true(json_object_get_null_member(
      json_object_get_object_member(channels, non_boost_slots[i]), "boost"));

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void _assert_mask_mode_schema(dt_iop_module_t *module,
                                     uint32_t stored,
                                     const char *const *expected,
                                     guint expected_len,
                                     gboolean writable)
{
  module->blend_params->mask_mode = stored;
  JsonNode *node = dt_remote_blend_schema(module);
  JsonObject *mm = json_object_get_object_member(_node_object(node), "mask_mode");
  JsonArray *values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(values), expected_len);
  for(guint i = 0; i < expected_len; i++)
    assert_string_equal(json_array_get_string_element(values, i), expected[i]);
  assert_int_equal(json_object_get_boolean_member(mm, "writable"), writable);
  json_node_unref(node);
}

static void test_schema_mask_mode_state_aware(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  static const char *const plain[] = { "off", "uniform", "parametric" };
  static const char *const drawn[] = { "drawn", "drawn+parametric" };
  static const char *const raster[] = { "raster" };

  _assert_mask_mode_schema(fx->module, DEVELOP_MASK_DISABLED, plain, 3, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL,
                           plain, 3, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK,
                           drawn, 2, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL,
                           drawn, 2, TRUE);
  _assert_mask_mode_schema(fx->module,
                           DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER,
                           raster, 1, FALSE);
  blend_fixture_free(fx);
}

static void test_schema_no_parametric_for_raw_module(void **state)
{
  (void)state;
  // Force the blending exposure fixture into RAW to exercise the family
  // gate without depending on a non-blending module's NULL schema.
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RAW;
  JsonNode *node = dt_remote_blend_schema(fx->module);
  JsonObject *schema = _node_object(node);
  assert_false(json_object_has_member(schema, "parametric"));
  assert_false(json_object_has_member(schema, "combine"));
  json_node_unref(node);

  // A legacy conditional bit remains representable/removable, but RAW
  // still does not expose writable channel/combine members.
  fx->module->blend_params->mask_mode =
    DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  node = dt_remote_blend_schema(fx->module);
  schema = _node_object(node);
  JsonObject *mm = json_object_get_object_member(schema, "mask_mode");
  JsonArray *values = json_object_get_array_member(mm, "values");
  assert_int_equal(json_array_get_length(values), 3);
  assert_string_equal(json_array_get_string_element(values, 2), "parametric");
  assert_true(json_object_get_boolean_member(mm, "writable"));
  assert_false(json_object_has_member(schema, "parametric"));
  assert_false(json_object_has_member(schema, "combine"));
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

/* ------------------------------------------------------------------ */
/* Task 3: parametric + combine read                                   */
/* ------------------------------------------------------------------ */

// helper: set a slot's markers + enable + polarity + boost directly
static void _set_slot(dt_develop_blend_params_t *bp, int slot,
                      float m0, float m1, float m2, float m3,
                      gboolean inverted, float boost)
{
  float *p = &bp->blendif_parameters[4 * slot];
  p[0] = m0; p[1] = m1; p[2] = m2; p[3] = m3;
  bp->blendif = dt_remote_blendif_slot_pack(bp->blendif, slot,
                                            dt_remote_blendif_markers_enable(p), inverted);
  bp->blendif_boost_factors[slot] = boost;
}

static void test_read_parametric_enabled_only(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  bp->mask_combine = DEVELOP_COMBINE_NORM_EXCL;
  // g_in enabled (non-identity), inverted; R_in left full-span => disabled
  _set_slot(bp, DEVELOP_BLENDIF_GRAY_in, 0.1f, 0.2f, 0.6f, 0.7f, TRUE, 0.0f);
  _set_slot(bp, DEVELOP_BLENDIF_RED_in, 0.0f, 0.0f, 1.0f, 1.0f, FALSE, 0.0f);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_string_equal(json_object_get_string_member(o, "mask_mode"), "parametric");
  assert_string_equal(json_object_get_string_member(o, "combine"), "exclusive");
  assert_false(json_object_has_member(o, "allow_inverted_combine")); // write-only

  JsonObject *param = json_object_get_object_member(o, "parametric");
  assert_true(json_object_has_member(param, "g_in"));
  assert_false(json_object_has_member(param, "R_in"));   // full-span -> disabled -> absent
  JsonObject *g = json_object_get_object_member(param, "g_in");
  JsonArray *markers = json_object_get_array_member(g, "markers");
  assert_float_equal(json_array_get_double_element(markers, 0), 0.1, 1e-6);
  assert_true(json_object_get_boolean_member(g, "inverted"));
  assert_float_equal(json_object_get_double_member(g, "boost"), 0.0, 1e-6);
  assert_false(json_object_get_boolean_member(o, "foreign_channels"));

  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_foreign_channels_flag(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;   // family = 0x77FF
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  // enable a Lab-only slot (bit 8 = C_in, in 0x3377 but the scene family
  // mask 0x77FF also includes bit 8; choose an out-of-0x77FF slot instead:
  // bit 15 DEVELOP_BLENDIF_unused is outside both). Use raw bit 11, which
  // is set in neither Lab_MASK (0x3377) nor RGB_MASK (0x77FF).
  _set_slot(bp, 11, 0.1f, 0.2f, 0.6f, 0.7f, FALSE, 0.0f);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *o = _node_object(node);
  assert_true(json_object_get_boolean_member(o, "foreign_channels"));
  json_node_unref(node);

  // Reserved slot 15 is not a foreign channel. Its polarity twin is the
  // legacy active bit 31; neither half may make the read flag true.
  bp->blendif = (1u << DEVELOP_BLENDIF_unused) | (1u << DEVELOP_BLENDIF_active);
  node = dt_remote_blend_read(fx->module);
  o = _node_object(node);
  assert_false(json_object_get_boolean_member(o, "foreign_channels"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

static void test_read_non_finite_parametric_storage_serializes_null(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  dt_develop_blend_params_t *bp = fx->module->blend_params;
  bp->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  bp->mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  _set_slot(bp, DEVELOP_BLENDIF_Jz_in, NAN, 0.2f, 0.6f, 0.7f,
            FALSE, INFINITY);

  JsonNode *node = dt_remote_blend_read(fx->module);
  JsonObject *entry = json_object_get_object_member(
    json_object_get_object_member(_node_object(node), "parametric"), "Jz_in");
  JsonArray *markers = json_object_get_array_member(entry, "markers");
  assert_true(json_node_is_null(json_array_get_element(markers, 0)));
  assert_true(json_object_get_null_member(entry, "boost"));
  json_node_unref(node);
  blend_fixture_free(fx);
}

/* ------------------------------------------------------------------ */
/* Task 4: patch apply                                                 */
/* ------------------------------------------------------------------ */

static JsonObject *_patch_from_string(const char *json)
{
  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  JsonObject *o = json_node_dup_object(json_parser_get_root(parser));
  g_object_unref(parser);
  return o;
}

static void _assert_patch_fails(blend_fixture_t *fx, const char *patch_json,
                                dt_remote_error_code_t code, const char *constraint)
{
  JsonObject *patch = _patch_from_string(patch_json);
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_non_null(error);
  assert_int_equal(error->code, code);
  if(constraint)
  {
    assert_non_null(error->details_json);
    assert_non_null(strstr(error->details_json, constraint));
  }
  dt_remote_error_free(error);
  json_object_unref(patch);
}

static void test_patch_opacity_and_mask_mode(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"mask_mode\":\"uniform\",\"opacity\":50.0}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_null(error);
  assert_int_equal(dst.mask_mode, DEVELOP_MASK_ENABLED);
  assert_float_equal(dst.opacity, 50.0f, 1e-6);
  // untouched members stay untouched
  assert_float_equal(dst.feathering_radius, fx->module->blend_params->feathering_radius, 1e-6);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_unknown_member_and_empty(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  _assert_patch_fails(fx, "{\"blendif\":3}", DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL);
  _assert_patch_fails(fx, "{}", DT_REMOTE_ERR_INVALID_VALUE, "empty");
  blend_fixture_free(fx);
}

// Mechanical M-A projection of the transition appendix: stored state x
// target in {"off","uniform"}. Rows off/uniform succeed (including the
// no-op diagonal); every masked row refuses with
// mask_configuration_present.
static void test_patch_mask_mode_transition_table(void **state)
{
  (void)state;
  const struct { uint32_t stored; gboolean writable; } rows[] = {
    { DEVELOP_MASK_DISABLED, TRUE },
    { DEVELOP_MASK_ENABLED, TRUE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL, FALSE },
    { DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER, FALSE },
  };
  const struct { const char *target; uint32_t bits; } targets[] = {
    { "off", DEVELOP_MASK_DISABLED }, { "uniform", DEVELOP_MASK_ENABLED },
  };
  blend_fixture_t *fx = blend_fixture_new("exposure");
  for(guint r = 0; r < G_N_ELEMENTS(rows); r++)
    for(guint t = 0; t < G_N_ELEMENTS(targets); t++)
    {
      fx->module->blend_params->mask_mode = rows[r].stored;
      gchar *json = g_strdup_printf("{\"mask_mode\":\"%s\"}", targets[t].target);
      if(rows[r].writable)
      {
        JsonObject *patch = _patch_from_string(json);
        dt_develop_blend_params_t dst = *fx->module->blend_params;
        dt_remote_error_t *error = NULL;
        assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
        assert_int_equal(dst.mask_mode, targets[t].bits);
        json_object_unref(patch);
      }
      else
        _assert_patch_fails(fx, json, DT_REMOTE_ERR_INVALID_VALUE, "mask_configuration_present");
      g_free(json);
    }
  fx->module->blend_params->mask_mode = DEVELOP_MASK_DISABLED;
  _assert_patch_fails(fx, "{\"mask_mode\":\"drawn\"}", DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_colorspace_reset_then_overrides(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // start from a non-default state so the reset is observable
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_DISPLAY;
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_HSV_VALUE | DEVELOP_BLEND_REVERSE;
  fx->module->blend_params->blend_parameter = 2.5f;

  // switch alone: mode/reverse/fulcrum reset to defaults for the new space
  JsonObject *patch = _patch_from_string("{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_cst, DEVELOP_BLEND_CS_RGB_SCENE);
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_NORMAL2);
  assert_float_equal(dst.blend_parameter, 0.0f, 1e-6);
  json_object_unref(patch);

  // switch + same-patch overrides: deterministic composition
  patch = _patch_from_string(
    "{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\",\"mode\":\"DEVELOP_BLEND_MULTIPLY\",\"fulcrum\":1.5}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_MULTIPLY);
  assert_float_equal(dst.blend_parameter, 1.5f, 1e-6);
  json_object_unref(patch);

  // writing the ALREADY-stored colorspace must NOT reset anything
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  fx->module->blend_params->blend_mode = DEVELOP_BLEND_MULTIPLY;
  patch = _patch_from_string("{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_SCENE\"}");
  dst = *fx->module->blend_params;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode, DEVELOP_BLEND_MULTIPLY);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_colorspace_rejections(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // exposure is RGB-default: Lab is not in its choice set
  _assert_patch_fails(fx, "{\"colorspace\":\"DEVELOP_BLEND_CS_LAB\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "colorspace_not_available");
  _assert_patch_fails(fx, "{\"colorspace\":\"DEVELOP_BLEND_CS_NONE\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "colorspace_not_available");
  _assert_patch_fails(fx, "{\"colorspace\":\"bogus\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_mode_validated_against_projected_colorspace(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  fx->module->blend_params->blend_cst = DEVELOP_BLEND_CS_RGB_SCENE;
  // HSV_VALUE exists only in RGB display -> rejected in scene space
  _assert_patch_fails(fx, "{\"mode\":\"DEVELOP_BLEND_HSV_VALUE\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "mode_not_available_in_colorspace");
  // deprecated modes are never writable
  _assert_patch_fails(fx, "{\"mode\":\"DEVELOP_BLEND_LAB_L\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "mode_not_available_in_colorspace");
  _assert_patch_fails(fx, "{\"mode\":\"nonsense\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  // projection: colorspace + mode in one patch validates mode against the NEW space
  JsonObject *patch = _patch_from_string(
    "{\"colorspace\":\"DEVELOP_BLEND_CS_RGB_DISPLAY\",\"mode\":\"DEVELOP_BLEND_HSV_VALUE\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.blend_mode & DEVELOP_BLEND_MODE_MASK, DEVELOP_BLEND_HSV_VALUE);
  json_object_unref(patch);
  blend_fixture_free(fx);
}

static void test_patch_reverse_flag(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"reverse\":true}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_true((dst.blend_mode & DEVELOP_BLEND_REVERSE) != 0);
  assert_int_equal(dst.blend_mode & DEVELOP_BLEND_MODE_MASK,
                   fx->module->blend_params->blend_mode & DEVELOP_BLEND_MODE_MASK);
  json_object_unref(patch);
  patch = _patch_from_string("{\"reverse\":false}");
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_true((dst.blend_mode & DEVELOP_BLEND_REVERSE) == 0);
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"reverse\":1}", DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  blend_fixture_free(fx);
}

static void test_patch_numeric_edges(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // hard-range edges are inclusive
  JsonObject *patch = _patch_from_string(
    "{\"opacity\":0.0,\"fulcrum\":-18.0,\"feathering_radius\":250.0,\"blur_radius\":100.0,"
    "\"contrast\":1.0,\"brightness\":-1.0}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_float_equal(dst.opacity, 0.0f, 1e-6);
  assert_float_equal(dst.blend_parameter, -18.0f, 1e-6);
  assert_float_equal(dst.feathering_radius, 250.0f, 1e-6);
  json_object_unref(patch);
  // soft range is NOT a write limit: fulcrum 10 (beyond soft 3) is legal
  patch = _patch_from_string("{\"fulcrum\":10.0}");
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"opacity\":100.001}", DT_REMOTE_ERR_INVALID_VALUE, "range");
  _assert_patch_fails(fx, "{\"opacity\":-0.001}", DT_REMOTE_ERR_INVALID_VALUE, "range");
  _assert_patch_fails(fx, "{\"opacity\":\"high\"}", DT_REMOTE_ERR_INVALID_VALUE, "wrong_type");
  blend_fixture_free(fx);
}

static void test_patch_feathering_guide(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  JsonObject *patch = _patch_from_string("{\"feathering_guide\":\"DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR\"}");
  dt_develop_blend_params_t dst = *fx->module->blend_params;
  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_blend_patch_apply(fx->module, patch, &dst, &error));
  assert_int_equal(dst.feathering_guide, DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR);
  json_object_unref(patch);
  _assert_patch_fails(fx, "{\"feathering_guide\":\"sideways\"}",
                      DT_REMOTE_ERR_INVALID_VALUE, "unknown_value");
  blend_fixture_free(fx);
}

static void test_patch_details_requires_raw_image(void **state)
{
  (void)state;
  blend_fixture_t *fx = blend_fixture_new("exposure");
  // fixture dev has no image -> details is never writable here
  _assert_patch_fails(fx, "{\"details\":0.5}", DT_REMOTE_ERR_INVALID_VALUE, "requires_raw_image");
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
    cmocka_unit_test(test_channels_parity_lab),
    cmocka_unit_test(test_channels_parity_rgb_display),
    cmocka_unit_test(test_channels_parity_rgb_scene),
    cmocka_unit_test(test_channels_none_and_raw_unsupported),
    cmocka_unit_test(test_boost_offsets_and_ranges),
    cmocka_unit_test(test_slot_enabled_inverted_pack),
    cmocka_unit_test(test_markers_enable_rule),
    cmocka_unit_test(test_mask_mode_targets_and_transition_matrix),
    cmocka_unit_test(test_schema_null_for_non_blending_module),
    cmocka_unit_test(test_schema_shape_for_rgb_module),
    cmocka_unit_test(test_schema_mask_mode_extra_bits_metadata),
    cmocka_unit_test(test_schema_parametric_for_rgb_scene),
    cmocka_unit_test(test_schema_parametric_for_rgb_display),
    cmocka_unit_test(test_schema_mask_mode_state_aware),
    cmocka_unit_test(test_schema_no_parametric_for_raw_module),
    cmocka_unit_test(test_read_defaults),
    cmocka_unit_test(test_read_reverse_and_deprecated_mode),
    cmocka_unit_test(test_read_non_finite_serializes_null),
    cmocka_unit_test(test_read_parametric_enabled_only),
    cmocka_unit_test(test_read_foreign_channels_flag),
    cmocka_unit_test(test_read_non_finite_parametric_storage_serializes_null),
    cmocka_unit_test(test_patch_opacity_and_mask_mode),
    cmocka_unit_test(test_patch_unknown_member_and_empty),
    cmocka_unit_test(test_patch_mask_mode_transition_table),
    cmocka_unit_test(test_patch_colorspace_reset_then_overrides),
    cmocka_unit_test(test_patch_colorspace_rejections),
    cmocka_unit_test(test_patch_mode_validated_against_projected_colorspace),
    cmocka_unit_test(test_patch_reverse_flag),
    cmocka_unit_test(test_patch_numeric_edges),
    cmocka_unit_test(test_patch_feathering_guide),
    cmocka_unit_test(test_patch_details_requires_raw_image),
  };
  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
