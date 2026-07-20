/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "control/remote_blend.h"

#include "common/darktable.h"
#include "common/image.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#include <math.h>
#include <string.h>

// Struct-churn tripwire: a DEVELOP_BLEND_VERSION bump means
// dt_develop_blend_params_t changed shape or meaning. Re-audit the field
// table below AND the wire contract (protocol reference SS Blend settings)
// before raising this guard. NOTE the guard covers layout only -- a new
// blend mode or a changed slider range bumps nothing; those are caught by
// the GUI-parity test (mode sets) and this file's range table review.
#if DEVELOP_BLEND_VERSION != 14
#error "dt_develop_blend_params_t changed: re-audit remote_blend.c's field table and the wire contract"
#endif

G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, mask_mode) == 0);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_cst) == 4);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_mode) == 8);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blend_parameter) == 12);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, opacity) == 16);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, mask_combine) == 20);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blendif) == 28);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, feathering_radius) == 32);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, feathering_guide) == 36);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blur_radius) == 40);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, contrast) == 44);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, brightness) == 48);
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, details) == 52);
G_STATIC_ASSERT(sizeof(((dt_develop_blend_params_t *)0)->blendif_parameters)
                == 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
G_STATIC_ASSERT(sizeof(((dt_develop_blend_params_t *)0)->blendif_boost_factors)
                == DEVELOP_BLENDIF_SIZE * sizeof(float));
G_STATIC_ASSERT(offsetof(dt_develop_blend_params_t, blendif_boost_factors)
                == offsetof(dt_develop_blend_params_t, blendif_parameters)
                   + 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

/* ------------------------------------------------------------------ */
/* enum name tables (wire = C enumerator names)                        */
/* ------------------------------------------------------------------ */

typedef struct _enum_name_t
{
  const char *name;
  uint32_t value;
} _enum_name_t;

static const _enum_name_t _colorspace_names[] G_GNUC_UNUSED = {
  { "DEVELOP_BLEND_CS_NONE", DEVELOP_BLEND_CS_NONE },
  { "DEVELOP_BLEND_CS_RAW", DEVELOP_BLEND_CS_RAW },
  { "DEVELOP_BLEND_CS_LAB", DEVELOP_BLEND_CS_LAB },
  { "DEVELOP_BLEND_CS_RGB_DISPLAY", DEVELOP_BLEND_CS_RGB_DISPLAY },
  { "DEVELOP_BLEND_CS_RGB_SCENE", DEVELOP_BLEND_CS_RGB_SCENE },
  { NULL, 0 } };

static const _enum_name_t _feathering_guide_names[] G_GNUC_UNUSED = {
  { "DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR", DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR },
  { "DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR", DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR },
  { "DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR", DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR },
  { "DEVELOP_MASK_GUIDE_IN_AFTER_BLUR", DEVELOP_MASK_GUIDE_IN_AFTER_BLUR },
  { NULL, 0 } };

static const _enum_name_t _mode_c_names[] = {
  { "DEVELOP_BLEND_DISABLED_OBSOLETE", DEVELOP_BLEND_DISABLED_OBSOLETE },
  { "DEVELOP_BLEND_NORMAL_OBSOLETE", DEVELOP_BLEND_NORMAL_OBSOLETE },
  { "DEVELOP_BLEND_LIGHTEN", DEVELOP_BLEND_LIGHTEN },
  { "DEVELOP_BLEND_DARKEN", DEVELOP_BLEND_DARKEN },
  { "DEVELOP_BLEND_MULTIPLY", DEVELOP_BLEND_MULTIPLY },
  { "DEVELOP_BLEND_AVERAGE", DEVELOP_BLEND_AVERAGE },
  { "DEVELOP_BLEND_ADD", DEVELOP_BLEND_ADD },
  { "DEVELOP_BLEND_SUBTRACT", DEVELOP_BLEND_SUBTRACT },
  { "DEVELOP_BLEND_DIFFERENCE", DEVELOP_BLEND_DIFFERENCE },
  { "DEVELOP_BLEND_SCREEN", DEVELOP_BLEND_SCREEN },
  { "DEVELOP_BLEND_OVERLAY", DEVELOP_BLEND_OVERLAY },
  { "DEVELOP_BLEND_SOFTLIGHT", DEVELOP_BLEND_SOFTLIGHT },
  { "DEVELOP_BLEND_HARDLIGHT", DEVELOP_BLEND_HARDLIGHT },
  { "DEVELOP_BLEND_VIVIDLIGHT", DEVELOP_BLEND_VIVIDLIGHT },
  { "DEVELOP_BLEND_LINEARLIGHT", DEVELOP_BLEND_LINEARLIGHT },
  { "DEVELOP_BLEND_PINLIGHT", DEVELOP_BLEND_PINLIGHT },
  { "DEVELOP_BLEND_LIGHTNESS", DEVELOP_BLEND_LIGHTNESS },
  { "DEVELOP_BLEND_CHROMATICITY", DEVELOP_BLEND_CHROMATICITY },
  { "DEVELOP_BLEND_HUE", DEVELOP_BLEND_HUE },
  { "DEVELOP_BLEND_COLOR", DEVELOP_BLEND_COLOR },
  { "DEVELOP_BLEND_INVERSE_OBSOLETE", DEVELOP_BLEND_INVERSE_OBSOLETE },
  { "DEVELOP_BLEND_UNBOUNDED_OBSOLETE", DEVELOP_BLEND_UNBOUNDED_OBSOLETE },
  { "DEVELOP_BLEND_COLORADJUST", DEVELOP_BLEND_COLORADJUST },
  { "DEVELOP_BLEND_DIFFERENCE2", DEVELOP_BLEND_DIFFERENCE2 },
  { "DEVELOP_BLEND_NORMAL2", DEVELOP_BLEND_NORMAL2 },
  { "DEVELOP_BLEND_BOUNDED", DEVELOP_BLEND_BOUNDED },
  { "DEVELOP_BLEND_LAB_LIGHTNESS", DEVELOP_BLEND_LAB_LIGHTNESS },
  { "DEVELOP_BLEND_LAB_COLOR", DEVELOP_BLEND_LAB_COLOR },
  { "DEVELOP_BLEND_HSV_VALUE", DEVELOP_BLEND_HSV_VALUE },
  { "DEVELOP_BLEND_HSV_COLOR", DEVELOP_BLEND_HSV_COLOR },
  { "DEVELOP_BLEND_LAB_L", DEVELOP_BLEND_LAB_L },
  { "DEVELOP_BLEND_LAB_A", DEVELOP_BLEND_LAB_A },
  { "DEVELOP_BLEND_LAB_B", DEVELOP_BLEND_LAB_B },
  { "DEVELOP_BLEND_RGB_R", DEVELOP_BLEND_RGB_R },
  { "DEVELOP_BLEND_RGB_G", DEVELOP_BLEND_RGB_G },
  { "DEVELOP_BLEND_RGB_B", DEVELOP_BLEND_RGB_B },
  { "DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE", DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE },
  { "DEVELOP_BLEND_SUBTRACT_INVERSE", DEVELOP_BLEND_SUBTRACT_INVERSE },
  { "DEVELOP_BLEND_DIVIDE", DEVELOP_BLEND_DIVIDE },
  { "DEVELOP_BLEND_DIVIDE_INVERSE", DEVELOP_BLEND_DIVIDE_INVERSE },
  { "DEVELOP_BLEND_GEOMETRIC_MEAN", DEVELOP_BLEND_GEOMETRIC_MEAN },
  { "DEVELOP_BLEND_HARMONIC_MEAN", DEVELOP_BLEND_HARMONIC_MEAN },
  { NULL, 0 } };

static const char *_enum_name_for_value(const _enum_name_t *table, uint32_t value)
{
  for(const _enum_name_t *e = table; e->name; e++)
    if(e->value == value) return e->name;
  return NULL;
}

static gboolean G_GNUC_UNUSED _enum_value_for_name(const _enum_name_t *table, const char *name,
                                                    uint32_t *out)
{
  if(!name) return FALSE;
  for(const _enum_name_t *e = table; e->name; e++)
    if(!strcmp(e->name, name)) { *out = e->value; return TRUE; }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* float field table                                                   */
/* ------------------------------------------------------------------ */

typedef struct _float_field_t
{
  const char *name;
  size_t offset;
  float min, max;
  float soft_min, soft_max;
  const char *unit;
} _float_field_t;

static const _float_field_t _float_fields[] G_GNUC_UNUSED = {
  { "fulcrum", offsetof(dt_develop_blend_params_t, blend_parameter), -18.0f, 18.0f, -3.0f, 3.0f, "EV" },
  { "opacity", offsetof(dt_develop_blend_params_t, opacity), 0.0f, 100.0f, 0.0f, 0.0f, "%" },
  { "feathering_radius", offsetof(dt_develop_blend_params_t, feathering_radius), 0.0f, 250.0f, 0.0f, 0.0f, "px" },
  { "blur_radius", offsetof(dt_develop_blend_params_t, blur_radius), 0.0f, 100.0f, 0.0f, 0.0f, "px" },
  { "contrast", offsetof(dt_develop_blend_params_t, contrast), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { "brightness", offsetof(dt_develop_blend_params_t, brightness), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { "details", offsetof(dt_develop_blend_params_t, details), -1.0f, 1.0f, 0.0f, 0.0f, NULL },
  { NULL, 0, 0, 0, 0, 0, NULL } };

/* ------------------------------------------------------------------ */
/* pure helpers                                                        */
/* ------------------------------------------------------------------ */

const char *dt_remote_blend_mask_mode_string(uint32_t mask_mode)
{
  if(mask_mode & DEVELOP_MASK_RASTER) return "raster";
  const gboolean drawn = (mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean parametric = (mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  if(drawn && parametric) return "drawn+parametric";
  if(drawn) return "drawn";
  if(parametric) return "parametric";
  return (mask_mode & DEVELOP_MASK_ENABLED) ? "uniform" : "off";
}

static gboolean _mask_mode_target_from_string(const char *s, uint32_t *out)
{
  if(!s || !out) return FALSE;
  if(!strcmp(s, "off")) { *out = DEVELOP_MASK_DISABLED; return TRUE; }
  if(!strcmp(s, "uniform")) { *out = DEVELOP_MASK_ENABLED; return TRUE; }
  if(!strcmp(s, "parametric"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
    return TRUE;
  }
  if(!strcmp(s, "drawn"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK;
    return TRUE;
  }
  if(!strcmp(s, "drawn+parametric"))
  {
    *out = DEVELOP_MASK_ENABLED | DEVELOP_MASK_MASK_CONDITIONAL;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out)
{
  return _mask_mode_target_from_string(s, out);
}

gboolean dt_remote_blend_mask_mode_transition(uint32_t stored,
                                              const char *target_name,
                                              uint32_t *out,
                                              const char **constraint)
{
  if(constraint) *constraint = NULL;
  if(stored & DEVELOP_MASK_RASTER)
  {
    if(constraint) *constraint = "raster_unsupported";
    return FALSE;
  }

  uint32_t target = 0;
  if(!_mask_mode_target_from_string(target_name, &target))
  {
    if(constraint) *constraint = "unknown_value";
    return FALSE;
  }
  if((stored & DEVELOP_MASK_MASK) != (target & DEVELOP_MASK_MASK))
  {
    if(constraint) *constraint = "drawn_via_attach_only";
    return FALSE;
  }

  *out = (stored & ~(DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL))
         | (target & (DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL));
  return TRUE;
}

/* ------------------------------------------------------------------ */
/* Tier 2: blendif channel tables (wire names are stable ASCII)        */
/* ------------------------------------------------------------------ */

// display_factor/display_unit are NON-NORMATIVE metadata (clients that
// want GUI-style numbers): display ~= (stored - marker_offset) * 2^boost
// * display_factor. The wire itself always carries stored 0..1 values.
static const dt_remote_blendif_channel_t _channels_lab[] = {
  { "L", DEVELOP_BLENDIF_L_in, DEVELOP_BLENDIF_L_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "a", DEVELOP_BLENDIF_A_in, DEVELOP_BLENDIF_A_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "b", DEVELOP_BLENDIF_B_in, DEVELOP_BLENDIF_B_out, TRUE,  0.0f, 0.5f, 256.0f, "" },
  { "C", DEVELOP_BLENDIF_C_in, DEVELOP_BLENDIF_C_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "h", DEVELOP_BLENDIF_h_in, DEVELOP_BLENDIF_h_out, FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_display[] = {
  { "g", DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "R", DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "G", DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "B", DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f, 0.0f, 100.0f, "%" },
  { "H", DEVELOP_BLENDIF_H_in,     DEVELOP_BLENDIF_H_out,     FALSE, 0.0f, 0.0f, 360.0f, "\xc2\xb0" },
  { "S", DEVELOP_BLENDIF_S_in,     DEVELOP_BLENDIF_S_out,     FALSE, 0.0f, 0.0f, 100.0f, "%" },
  { "l", DEVELOP_BLENDIF_l_in,     DEVELOP_BLENDIF_l_out,     FALSE, 0.0f, 0.0f, 100.0f, "%" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

static const dt_remote_blendif_channel_t _channels_rgb_scene[] = {
  { "g",  DEVELOP_BLENDIF_GRAY_in,  DEVELOP_BLENDIF_GRAY_out,  TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "R",  DEVELOP_BLENDIF_RED_in,   DEVELOP_BLENDIF_RED_out,   TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "G",  DEVELOP_BLENDIF_GREEN_in, DEVELOP_BLENDIF_GREEN_out, TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "B",  DEVELOP_BLENDIF_BLUE_in,  DEVELOP_BLENDIF_BLUE_out,  TRUE,  0.0f,         0.0f, 100.0f, "%" },
  { "Jz", DEVELOP_BLENDIF_Jz_in,    DEVELOP_BLENDIF_Jz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "%" },
  { "Cz", DEVELOP_BLENDIF_Cz_in,    DEVELOP_BLENDIF_Cz_out,    TRUE,  -6.64385619f, 0.0f, 100.0f, "%" },
  { "hz", DEVELOP_BLENDIF_hz_in,    DEVELOP_BLENDIF_hz_out,    FALSE, 0.0f,         0.0f, 360.0f, "\xc2\xb0" },
  { NULL, 0, 0, FALSE, 0.0f, 0.0f, 0.0f, NULL } };

const dt_remote_blendif_channel_t *
dt_remote_blendif_channels(dt_develop_blend_colorspace_t csp)
{
  switch(csp)
  {
    case DEVELOP_BLEND_CS_LAB:         return _channels_lab;
    case DEVELOP_BLEND_CS_RGB_DISPLAY: return _channels_rgb_display;
    case DEVELOP_BLEND_CS_RGB_SCENE:   return _channels_rgb_scene;
    default:                           return NULL;  // RAW/NONE: no parametric channel table
  }
}

gboolean dt_remote_blendif_slot_enabled(uint32_t blendif, int slot)
{
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, FALSE);
  return (blendif & (1u << slot)) != 0;
}

gboolean dt_remote_blendif_slot_inverted(uint32_t blendif, int slot)
{
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, FALSE);
  return (blendif & (1u << (16 + slot))) != 0;
}

uint32_t dt_remote_blendif_slot_pack(uint32_t blendif, int slot,
                                     gboolean enabled, gboolean inverted)
{
  blendif &= ~(1u << 31);              // strip legacy DEVELOP_BLENDIF_active
  g_return_val_if_fail(slot >= 0 && slot < DEVELOP_BLENDIF_unused, blendif);
  if(enabled)  blendif |=  (1u << slot);        else blendif &= ~(1u << slot);
  if(inverted) blendif |=  (1u << (16 + slot)); else blendif &= ~(1u << (16 + slot));
  return blendif;
}

gboolean dt_remote_blendif_markers_enable(const float m[4])
{
  return !(m[1] == 0.0f && m[2] == 1.0f);
}

dt_develop_blend_colorspace_t
dt_remote_blend_effective_colorspace(struct dt_iop_module_t *module, int32_t stored_cst)
{
  switch(stored_cst)
  {
    case DEVELOP_BLEND_CS_RAW:
    case DEVELOP_BLEND_CS_LAB:
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return stored_cst;
    default:
      return dt_develop_blend_default_module_blend_colorspace(module);
  }
}

GPtrArray *dt_remote_blend_mode_names_for_colorspace(dt_develop_blend_colorspace_t csp)
{
  GPtrArray *out = g_ptr_array_new();
  for(const dt_develop_blend_mode_section_t *s = dt_develop_blend_mode_sections(csp);
      s && s->section; s++)
  {
    const dt_introspection_type_enum_tuple_t *item = dt_develop_blend_mode_names;
    while(item->name && item->value != (int)s->from) item++;
    for(; item->name; item++)
    {
      const char *cname = _enum_name_for_value(_mode_c_names, (uint32_t)item->value);
      if(cname) g_ptr_array_add(out, (gpointer)cname);
      if(item->value == (int)s->to) break;
    }
  }
  return out;
}

/* ------------------------------------------------------------------ */
/* schema + read                                                       */
/* ------------------------------------------------------------------ */

// Choice set per the GUI colorspace menu: modules whose default space is
// Lab/RGB may choose; RAW and CS_NONE defaults have no choice.
static guint _colorspace_choices(dt_iop_module_t *module,
                                 dt_develop_blend_colorspace_t *choices,
                                 gboolean *writable)
{
  const dt_develop_blend_colorspace_t def =
    dt_develop_blend_default_module_blend_colorspace(module);
  guint n = 0;
  if(def == DEVELOP_BLEND_CS_LAB
     || def == DEVELOP_BLEND_CS_RGB_DISPLAY
     || def == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    *writable = TRUE;
    if(def == DEVELOP_BLEND_CS_LAB) choices[n++] = DEVELOP_BLEND_CS_LAB;
    choices[n++] = DEVELOP_BLEND_CS_RGB_DISPLAY;
    choices[n++] = DEVELOP_BLEND_CS_RGB_SCENE;
    return n;
  }
  *writable = FALSE;
  choices[n++] = def;
  return n;
}

static gboolean _details_writable(dt_iop_module_t *module)
{
  return module->dev
         && dt_is_valid_imgid(module->dev->image_storage.id)
         && dt_image_is_rawprepare_supported(&module->dev->image_storage);
}

static void _add_float_member(JsonBuilder *b, const char *name, float value)
{
  json_builder_set_member_name(b, name);
  if(isfinite(value)) json_builder_add_double_value(b, (double)value);
  else json_builder_add_null_value(b);
}

JsonNode *dt_remote_blend_schema(dt_iop_module_t *module)
{
  if(!module || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return NULL;
  const dt_develop_blend_params_t *bp = module->blend_params;
  const gboolean extra_bits =
    (bp->mask_mode & (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL | DEVELOP_MASK_RASTER)) != 0;
  const dt_develop_blend_colorspace_t eff_mm =
    dt_remote_blend_effective_colorspace(module, module->blend_params->blend_cst);
  const gboolean parametric_supported = dt_remote_blendif_channels(eff_mm) != NULL;
  const gboolean raster = (bp->mask_mode & DEVELOP_MASK_RASTER) != 0;
  const gboolean drawn = (bp->mask_mode & DEVELOP_MASK_MASK) != 0;
  const gboolean conditional = (bp->mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "mask_mode");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  if(raster)
    json_builder_add_string_value(b, "raster");
  else if(drawn)
  {
    json_builder_add_string_value(b, "drawn");
    // Keep a legacy unsupported current value representable so it can be
    // removed, even though it cannot be newly added in this family.
    if(parametric_supported || conditional)
      json_builder_add_string_value(b, "drawn+parametric");
  }
  else
  {
    json_builder_add_string_value(b, "off");
    json_builder_add_string_value(b, "uniform");
    if(parametric_supported || conditional)
      json_builder_add_string_value(b, "parametric");
  }
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(
    b, !raster && (!drawn || parametric_supported || conditional));
  json_builder_set_member_name(b, "current_extra_bits");
  json_builder_add_boolean_value(b, extra_bits);
  json_builder_end_object(b);

  dt_develop_blend_colorspace_t choices[3];
  gboolean cs_writable = FALSE;
  const guint n_choices = _colorspace_choices(module, choices, &cs_writable);
  json_builder_set_member_name(b, "colorspace");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(guint i = 0; i < n_choices; i++)
    json_builder_add_string_value(b, _enum_name_for_value(_colorspace_names, choices[i]));
  json_builder_end_array(b);
  json_builder_set_member_name(b, "default");
  json_builder_add_string_value(b,
    _enum_name_for_value(_colorspace_names,
                         dt_develop_blend_default_module_blend_colorspace(module)));
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, cs_writable);
  json_builder_end_object(b);

  GPtrArray *mode_names = dt_remote_blend_mode_names_for_colorspace(eff_mm);
  json_builder_set_member_name(b, "mode");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(guint i = 0; i < mode_names->len; i++)
    json_builder_add_string_value(b, g_ptr_array_index(mode_names, i));
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);
  g_ptr_array_unref(mode_names);

  json_builder_set_member_name(b, "reverse");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "bool");
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "feathering_guide");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, "enum");
  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(const _enum_name_t *e = _feathering_guide_names; e->name; e++)
    json_builder_add_string_value(b, e->name);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);

  for(const _float_field_t *f = _float_fields; f->name; f++)
  {
    json_builder_set_member_name(b, f->name);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "float");
    json_builder_set_member_name(b, "range");
    json_builder_begin_array(b);
    json_builder_add_double_value(b, (double)f->min);
    json_builder_add_double_value(b, (double)f->max);
    json_builder_end_array(b);
    if(f->soft_min != f->soft_max)
    {
      json_builder_set_member_name(b, "soft_range");
      json_builder_begin_array(b);
      json_builder_add_double_value(b, (double)f->soft_min);
      json_builder_add_double_value(b, (double)f->soft_max);
      json_builder_end_array(b);
    }
    if(f->unit)
    {
      json_builder_set_member_name(b, "unit");
      json_builder_add_string_value(b, f->unit);
    }
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b,
      strcmp(f->name, "details") ? TRUE : _details_writable(module));
    json_builder_end_object(b);
  }

  // Tier 2: combine + parametric (Lab / RGB families only; absent for RAW)
  if(parametric_supported)
  {
    json_builder_set_member_name(b, "allow_inverted_combine");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "bool");
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_set_member_name(b, "write_only");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "combine");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "type");
    json_builder_add_string_value(b, "enum");
    json_builder_set_member_name(b, "values");
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "exclusive");
    json_builder_add_string_value(b, "inclusive");
    json_builder_add_string_value(b, "exclusive_inverted");
    json_builder_add_string_value(b, "inclusive_inverted");
    json_builder_end_array(b);
    json_builder_set_member_name(b, "writable");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);

    json_builder_set_member_name(b, "parametric");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "channels");
    json_builder_begin_object(b);
    for(const dt_remote_blendif_channel_t *c = dt_remote_blendif_channels(eff_mm);
        c && c->name; c++)
    {
      for(int io = 0; io < 2; io++)
      {
        gchar *slot_name = g_strdup_printf("%s_%s", c->name, io ? "out" : "in");
        json_builder_set_member_name(b, slot_name);
        g_free(slot_name);
        json_builder_begin_object(b);

        json_builder_set_member_name(b, "markers_domain");
        json_builder_begin_array(b);
        json_builder_add_double_value(b, 0.0);
        json_builder_add_double_value(b, 1.0);
        json_builder_end_array(b);

        json_builder_set_member_name(b, "boost");
        if(c->boost_supported)
        {
          json_builder_begin_object(b);
          json_builder_set_member_name(b, "writable");
          json_builder_add_boolean_value(b, TRUE);
          json_builder_set_member_name(b, "offset");
          json_builder_add_double_value(b, (double)c->boost_offset);
          json_builder_set_member_name(b, "range");
          json_builder_begin_array(b);
          json_builder_add_double_value(b, (double)c->boost_offset);
          json_builder_add_double_value(b, (double)c->boost_offset + 18.0);
          json_builder_end_array(b);
          json_builder_end_object(b);
        }
        else
          json_builder_add_null_value(b);

        json_builder_set_member_name(b, "display_hint");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "factor");
        json_builder_add_double_value(b, (double)c->display_factor);
        json_builder_set_member_name(b, "offset");
        json_builder_add_double_value(b, (double)c->marker_offset);
        json_builder_set_member_name(b, "unit");
        json_builder_add_string_value(b, c->display_unit);
        json_builder_set_member_name(b, "boost_scales");
        json_builder_add_boolean_value(b, c->boost_supported);
        json_builder_end_object(b);

        json_builder_end_object(b);
      }
    }
    json_builder_end_object(b);  // channels
    json_builder_end_object(b);  // parametric
  }

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

static const char *_combine_to_string(uint32_t mask_combine)
{
  switch(mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL))
  {
    case DEVELOP_COMBINE_NORM_EXCL: return "exclusive";
    case DEVELOP_COMBINE_NORM_INCL: return "inclusive";
    case DEVELOP_COMBINE_INV_EXCL:  return "exclusive_inverted";
    case DEVELOP_COMBINE_INV_INCL:  return "inclusive_inverted";
    default:                        return "exclusive";
  }
}

static gboolean _combine_from_string(const char *s,
                                     uint32_t *bits /* INV|INCL only */)
{
  if(!s || !bits) return FALSE;
  if(!strcmp(s, "exclusive"))
    { *bits = DEVELOP_COMBINE_NORM_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive"))
    { *bits = DEVELOP_COMBINE_NORM_INCL; return TRUE; }
  if(!strcmp(s, "exclusive_inverted"))
    { *bits = DEVELOP_COMBINE_INV_EXCL; return TRUE; }
  if(!strcmp(s, "inclusive_inverted"))
    { *bits = DEVELOP_COMBINE_INV_INCL; return TRUE; }
  return FALSE;
}

JsonNode *dt_remote_blend_read(dt_iop_module_t *module)
{
  if(!module || !(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return NULL;
  const dt_develop_blend_params_t *bp = module->blend_params;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "mask_mode");
  json_builder_add_string_value(b, dt_remote_blend_mask_mode_string(bp->mask_mode));

  const dt_develop_blend_colorspace_t default_cst =
    dt_develop_blend_default_module_blend_colorspace(module);
  const int32_t stored_cst = bp->blend_cst == default_cst ? DEVELOP_BLEND_CS_NONE : bp->blend_cst;
  const char *cs_name = _enum_name_for_value(_colorspace_names, (uint32_t)stored_cst);
  json_builder_set_member_name(b, "colorspace");
  if(cs_name) json_builder_add_string_value(b, cs_name);
  else json_builder_add_int_value(b, stored_cst);

  json_builder_set_member_name(b, "effective_colorspace");
  json_builder_add_string_value(b,
    _enum_name_for_value(_colorspace_names,
                         dt_remote_blend_effective_colorspace(module, bp->blend_cst)));

  const uint32_t mode_value = bp->blend_mode & DEVELOP_BLEND_MODE_MASK;
  const char *mode_name = _enum_name_for_value(_mode_c_names, mode_value);
  json_builder_set_member_name(b, "mode");
  if(mode_name) json_builder_add_string_value(b, mode_name);
  else json_builder_add_int_value(b, (gint64)mode_value);

  json_builder_set_member_name(b, "reverse");
  json_builder_add_boolean_value(b, (bp->blend_mode & DEVELOP_BLEND_REVERSE) != 0);

  const char *fg_name = _enum_name_for_value(_feathering_guide_names, bp->feathering_guide);
  json_builder_set_member_name(b, "feathering_guide");
  if(fg_name) json_builder_add_string_value(b, fg_name);
  else json_builder_add_int_value(b, (gint64)bp->feathering_guide);

  for(const _float_field_t *f = _float_fields; f->name; f++)
    _add_float_member(b, f->name, *(const float *)((const guint8 *)bp + f->offset));

  const dt_develop_blend_colorspace_t eff_r =
    dt_remote_blend_effective_colorspace(module, bp->blend_cst);
  const dt_remote_blendif_channel_t *table = dt_remote_blendif_channels(eff_r);
  if(table)
  {
    json_builder_set_member_name(b, "combine");
    json_builder_add_string_value(b, _combine_to_string(bp->mask_combine));

    json_builder_set_member_name(b, "parametric");
    json_builder_begin_object(b);
    uint32_t in_family = 0u;
    for(const dt_remote_blendif_channel_t *c = table; c->name; c++)
    {
      const int slots[2] = { (int)c->slot_in, (int)c->slot_out };
      for(int io = 0; io < 2; io++)
      {
        const int slot = slots[io];
        in_family |= (1u << slot);
        if(!dt_remote_blendif_slot_enabled(bp->blendif, slot)) continue;
        gchar *slot_name = g_strdup_printf("%s_%s", c->name, io ? "out" : "in");
        json_builder_set_member_name(b, slot_name);
        g_free(slot_name);
        json_builder_begin_object(b);
        const float *p = &bp->blendif_parameters[4 * slot];
        json_builder_set_member_name(b, "markers");
        json_builder_begin_array(b);
        for(int k = 0; k < 4; k++)
        {
          if(isfinite(p[k])) json_builder_add_double_value(b, (double)p[k]);
          else json_builder_add_null_value(b); // corrupt legacy storage: keep JSON strict
        }
        json_builder_end_array(b);
        json_builder_set_member_name(b, "inverted");
        json_builder_add_boolean_value(b, dt_remote_blendif_slot_inverted(bp->blendif, slot));
        _add_float_member(b, "boost", bp->blendif_boost_factors[slot]);
        json_builder_end_object(b);
      }
    }
    json_builder_end_object(b);

    // foreign: any enabled slot outside the effective family (legacy edits)
    const uint32_t usable_slot_mask = (1u << DEVELOP_BLENDIF_unused) - 1u;
    const uint32_t enabled_slots = bp->blendif & usable_slot_mask;
    json_builder_set_member_name(b, "foreign_channels");
    json_builder_add_boolean_value(b, (enabled_slots & ~in_family) != 0);
  }

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

/* ------------------------------------------------------------------ */
/* patch apply                                                         */
/* ------------------------------------------------------------------ */

static dt_remote_error_t *_blend_error(dt_remote_error_code_t code,
                                       const char *member, const char *constraint,
                                       const char *fmt, ...) G_GNUC_PRINTF(4, 5);
static dt_remote_error_t *_blend_error(dt_remote_error_code_t code,
                                       const char *member, const char *constraint,
                                       const char *fmt, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = code;
  va_list args;
  va_start(args, fmt);
  error->message = g_strdup_vprintf(fmt, args);
  va_end(args);
  if(member && constraint)
    error->details_json =
      g_strdup_printf("{\"parameter\":\"blend.%s\",\"constraint\":\"%s\"}", member, constraint);
  else if(constraint)
    error->details_json =
      g_strdup_printf("{\"parameter\":\"blend\",\"constraint\":\"%s\"}", constraint);
  else if(member)
    error->details_json = g_strdup_printf("{\"parameter\":\"blend.%s\"}", member);
  return error;
}

static gboolean _json_member_double(JsonObject *o, const char *name, double *out)
{
  JsonNode *node = json_object_get_member(o, name);
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
  const GType t = json_node_get_value_type(node);
  if(t == G_TYPE_DOUBLE) { *out = json_node_get_double(node); return TRUE; }
  if(t == G_TYPE_INT64) { *out = (double)json_node_get_int(node); return TRUE; }
  return FALSE;
}

static const char *_json_member_string(JsonObject *o, const char *name)
{
  JsonNode *node = json_object_get_member(o, name);
  if(!node || !JSON_NODE_HOLDS_VALUE(node)
     || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

gboolean dt_remote_blend_patch_apply(dt_iop_module_t *module,
                                     JsonObject *patch,
                                     dt_develop_blend_params_t *dst,
                                     dt_remote_error_t **error)
{
  static const char *const allowed[] = {
    "mask_mode", "colorspace", "mode", "reverse", "fulcrum", "opacity",
    "feathering_radius", "feathering_guide", "blur_radius", "contrast",
    "brightness", "details",
    "combine", "parametric", "allow_inverted_combine", NULL };

  GList *members = json_object_get_members(patch);
  if(!members)
  {
    if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, NULL, "empty",
                                    _("'blend' must be non-empty"));
    return FALSE;
  }
  for(GList *m = members; m; m = m->next)
  {
    gboolean known = FALSE;
    for(const char *const *a = allowed; *a && !known; a++)
      known = !strcmp(*a, m->data);
    if(!known)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, (const char *)m->data, NULL,
                                      _("unknown blend member 'blend.%s'"), (const char *)m->data);
      g_list_free(members);
      return FALSE;
    }
  }
  g_list_free(members);

  // Capture, before any stage runs, whether a stored conditional bit was a
  // supported state or an unsupported legacy one. The projected-family gate
  // after the colorspace stage uses this to permit removing a legacy
  // CONDITIONAL while still forbidding a patch from creating one.
  const gboolean stored_conditional =
    (dst->mask_mode & DEVELOP_MASK_CONDITIONAL) != 0;
  const dt_develop_blend_colorspace_t stored_eff_cs =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  const gboolean stored_parametric_supported =
    dt_remote_blendif_channels(stored_eff_cs) != NULL;

  // 1. mask_mode -- authoritative appendix; only ENABLED|CONDITIONAL
  // may change, and MASK ownership must remain identical. This stage
  // projects bits only; family availability is deliberately checked
  // after the colorspace stage so a same-call RAW -> RGB switch works.
  if(json_object_has_member(patch, "mask_mode"))
  {
    const char *constraint = NULL;
    uint32_t projected = 0;
    if(!dt_remote_blend_mask_mode_transition(
         dst->mask_mode, _json_member_string(patch, "mask_mode"),
         &projected, &constraint))
    {
      const char *message = !strcmp(constraint, "raster_unsupported")
        ? _("raster masks are not writable")
        : !strcmp(constraint, "drawn_via_attach_only")
          ? _("drawn masks are entered and left through attach/detach")
          : _("unknown mask_mode target");
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE,
                                      "mask_mode", constraint, "%s", message);
      return FALSE;
    }
    dst->mask_mode = projected;
  }

  // 2. colorspace -- deterministic reset semantics (design decision 3):
  // a change resets mode/reverse/fulcrum/blendif to the new space's
  // defaults; later members of the SAME patch then apply on top. The
  // GUI's history-scavenging restore is deliberately not reproduced.
  if(json_object_has_member(patch, "colorspace"))
  {
    dt_develop_blend_colorspace_t choices[3];
    gboolean cs_writable = FALSE;
    const guint n_choices = _colorspace_choices(module, choices, &cs_writable);
    const char *cs_string = _json_member_string(patch, "colorspace");
    uint32_t cs_value = 0;
    if(!cs_string || !_enum_value_for_name(_colorspace_names, cs_string, &cs_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "colorspace", "unknown_value",
                                      _("unknown blend colorspace"));
      return FALSE;
    }
    gboolean in_choices = FALSE;
    for(guint i = 0; cs_writable && i < n_choices && !in_choices; i++)
      in_choices = (choices[i] == (dt_develop_blend_colorspace_t)cs_value);
    if(!in_choices)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "colorspace",
                                      "colorspace_not_available",
                                      _("blend colorspace not available for this module"));
      return FALSE;
    }
    if((int32_t)cs_value != dst->blend_cst)
      dt_develop_blend_init_blendif_parameters(dst, (dt_develop_blend_colorspace_t)cs_value);
  }

  // Projected-family gate. Derived from the FINAL blend_cst so a same-call
  // colorspace switch is honored. A conditional bit is unavailable in
  // RAW/NONE; permit an already-stored unsupported legacy CONDITIONAL to
  // remain representable or be removed, but never let a patch create such a
  // state or move one from a supported family into an unsupported one.
  const dt_develop_blend_colorspace_t eff =
    dt_remote_blend_effective_colorspace(module, dst->blend_cst);
  const dt_remote_blendif_channel_t *table = dt_remote_blendif_channels(eff);
  if((dst->mask_mode & DEVELOP_MASK_CONDITIONAL) && !table
     && (!stored_conditional || stored_parametric_supported))
  {
    const char *field = stored_conditional ? "colorspace" : "mask_mode";
    if(error) *error = _blend_error(
      DT_REMOTE_ERR_UNSUPPORTED_FIELD, field, NULL,
      _("parametric masks are unavailable in the projected blend colorspace"));
    return FALSE;
  }

  // 3a. allow_inverted_combine -- strict boolean override for the H4 guard.
  gboolean combine_changed = FALSE;
  gboolean allow_inv_combine = FALSE;
  if(json_object_has_member(patch, "allow_inverted_combine"))
  {
    JsonNode *node = json_object_get_member(patch, "allow_inverted_combine");
    if(!node || !JSON_NODE_HOLDS_VALUE(node)
       || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE,
                                      "allow_inverted_combine", "wrong_type",
                                      _("'allow_inverted_combine' must be a boolean"));
      return FALSE;
    }
    allow_inv_combine = json_node_get_boolean(node);
  }

  // 3b. combine
  if(json_object_has_member(patch, "combine"))
  {
    if(!table)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "combine", NULL,
                                      _("combine is not available for this blend colorspace"));
      return FALSE;
    }
    uint32_t bits = 0;
    if(!_combine_from_string(_json_member_string(patch, "combine"), &bits))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "combine", "unknown_value",
                                      _("unknown combine value"));
      return FALSE;
    }
    const uint32_t before = dst->mask_combine & (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL);
    combine_changed = (before != bits);
    dst->mask_combine =
      (dst->mask_combine & ~(DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)) | bits;  // MASKS_POS preserved
  }

  // 3c. parametric
  if(json_object_has_member(patch, "parametric"))
  {
    if(!table)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, "parametric", NULL,
                                      _("parametric masks are not available for this blend colorspace"));
      return FALSE;
    }
    if(!(dst->mask_mode & DEVELOP_MASK_CONDITIONAL))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric",
                                      "requires_parametric_mask_mode",
                                      _("set mask_mode to \"parametric\" to write parametric channels"));
      return FALSE;
    }
    JsonNode *pnode = json_object_get_member(patch, "parametric");
    if(!pnode || !JSON_NODE_HOLDS_OBJECT(pnode))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                      _("'parametric' must be an object"));
      return FALSE;
    }
    JsonObject *pobj = json_node_get_object(pnode);
    GList *names = json_object_get_members(pobj);
    for(GList *n = names; n; n = n->next)
    {
      const char *slot_name = n->data;
      // resolve slot_name -> channel + in/out
      const dt_remote_blendif_channel_t *ch = NULL;
      int slot = -1;
      for(const dt_remote_blendif_channel_t *c = table; c->name && slot < 0; c++)
      {
        gchar *in_name = g_strdup_printf("%s_in", c->name);
        gchar *out_name = g_strdup_printf("%s_out", c->name);
        if(!strcmp(slot_name, in_name))  { ch = c; slot = (int)c->slot_in; }
        else if(!strcmp(slot_name, out_name)) { ch = c; slot = (int)c->slot_out; }
        g_free(in_name); g_free(out_name);
      }
      if(!ch)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL, NULL,
                                        _("unknown parametric channel"));
        if(error && *error)
        {
          g_free((*error)->details_json);
          (*error)->details_json = g_strdup_printf(
            "{\"parameter\":\"blend.parametric.%s\"}", slot_name);
        }
        g_list_free(names);
        return FALSE;
      }
      JsonNode *entry = json_object_get_member(pobj, slot_name);
      float *p = &dst->blendif_parameters[4 * slot];
      if(json_node_is_null(entry))
      {
        // null resets: identity markers, polarity cleared, boost -> channel offset
        p[0] = 0.0f; p[1] = 0.0f; p[2] = 1.0f; p[3] = 1.0f;
        dst->blendif = dt_remote_blendif_slot_pack(dst->blendif, slot, FALSE, FALSE);
        dst->blendif_boost_factors[slot] = ch->boost_offset;
        continue;
      }
      if(!JSON_NODE_HOLDS_OBJECT(entry))
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                        _("a parametric channel entry must be an object or null"));
        g_list_free(names);
        return FALSE;
      }
      JsonObject *e = json_node_get_object(entry);
      static const char *const entry_allowed[] = { "markers", "inverted", "boost", NULL };
      GList *entry_members = json_object_get_members(e);
      for(GList *em = entry_members; em; em = em->next)
      {
        gboolean known = FALSE;
        for(const char *const *a = entry_allowed; *a && !known; a++)
          known = !strcmp(*a, em->data);
        if(!known)
        {
          if(error)
          {
            *error = _blend_error(DT_REMOTE_ERR_UNSUPPORTED_FIELD, NULL, NULL,
                                  _("unknown parametric channel member"));
            g_free((*error)->details_json);
            (*error)->details_json = g_strdup_printf(
              "{\"parameter\":\"blend.parametric.%s.%s\"}",
              slot_name, (const char *)em->data);
          }
          g_list_free(entry_members);
          g_list_free(names);
          return FALSE;
        }
      }
      g_list_free(entry_members);

      // markers: exactly 4 finite ascending in [0,1]
      JsonNode *mnode = json_object_get_member(e, "markers");
      if(!mnode || !JSON_NODE_HOLDS_ARRAY(mnode)
         || json_array_get_length(json_node_get_array(mnode)) != 4)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                        _("'markers' must be an array of exactly 4 numbers"));
        g_list_free(names);
        return FALSE;
      }
      JsonArray *marr = json_node_get_array(mnode);
      float m[4];
      double prev = -G_MAXDOUBLE;
      for(int k = 0; k < 4; k++)
      {
        JsonNode *mk = json_array_get_element(marr, k);
        if(!JSON_NODE_HOLDS_VALUE(mk)
           || (json_node_get_value_type(mk) != G_TYPE_DOUBLE
               && json_node_get_value_type(mk) != G_TYPE_INT64))
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                          _("markers must be numbers"));
          g_list_free(names);
          return FALSE;
        }
        const double value = json_node_get_double(mk);
        if(!isfinite(value) || value < 0.0 || value > 1.0 || value < prev)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "markers",
                                          _("markers must be finite, ascending, within [0,1]"));
          g_list_free(names);
          return FALSE;
        }
        prev = value;
        m[k] = (float)value;  // narrow only after double-domain validation
      }
      // inverted (optional bool, default false)
      gboolean inverted = FALSE, has_inverted = FALSE;
      if(json_object_has_member(e, "inverted"))
      {
        JsonNode *inode = json_object_get_member(e, "inverted");
        if(!JSON_NODE_HOLDS_VALUE(inode) || json_node_get_value_type(inode) != G_TYPE_BOOLEAN)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "wrong_type",
                                          _("'inverted' must be a boolean"));
          g_list_free(names);
          return FALSE;
        }
        inverted = json_node_get_boolean(inode);
        has_inverted = TRUE;
      }
      // H4 guard: inverted change together with a combine change
      if(has_inverted && combine_changed && !allow_inv_combine)
      {
        if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric",
                                        "inverted_and_combine_conflict",
                                        _("changing 'inverted' and 'combine' together is "
                                          "error-prone; pass allow_inverted_combine:true to confirm"));
        g_list_free(names);
        return FALSE;
      }
      // boost (optional; default = channel offset i.e. GUI zero)
      float boost = ch->boost_offset;
      if(json_object_has_member(e, "boost"))
      {
        if(!ch->boost_supported)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("this channel does not support boost"));
          g_list_free(names);
          return FALSE;
        }
        double bv = 0.0;
        JsonNode *bnode = json_object_get_member(e, "boost");
        if(!JSON_NODE_HOLDS_VALUE(bnode)
           || (json_node_get_value_type(bnode) != G_TYPE_DOUBLE
               && json_node_get_value_type(bnode) != G_TYPE_INT64))
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("'boost' must be a number"));
          g_list_free(names);
          return FALSE;
        }
        bv = json_node_get_double(bnode);
        if(!isfinite(bv) || bv < (double)ch->boost_offset || bv > (double)ch->boost_offset + 18.0)
        {
          if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "parametric", "boost",
                                          _("'boost' out of range for this channel"));
          g_list_free(names);
          return FALSE;
        }
        boost = (float)bv;
      }
      // write verbatim; derive enable from markers (never rescale markers)
      for(int k = 0; k < 4; k++) p[k] = m[k];
      dst->blendif = dt_remote_blendif_slot_pack(dst->blendif, slot,
                                                 dt_remote_blendif_markers_enable(m), inverted);
      dst->blendif_boost_factors[slot] = boost;
    }
    g_list_free(names);
  }

  // 3. mode -- validated against the PROJECTED effective colorspace `eff`.
  if(json_object_has_member(patch, "mode"))
  {
    const char *mode_string = _json_member_string(patch, "mode");
    uint32_t mode_value = 0;
    if(!mode_string || !_enum_value_for_name(_mode_c_names, mode_string, &mode_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mode", "unknown_value",
                                      _("unknown blend mode"));
      return FALSE;
    }
    GPtrArray *names = dt_remote_blend_mode_names_for_colorspace(eff);
    gboolean available = FALSE;
    for(guint i = 0; i < names->len && !available; i++)
      available = !strcmp(g_ptr_array_index(names, i), mode_string);
    g_ptr_array_unref(names);
    if(!available)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "mode",
                                      "mode_not_available_in_colorspace",
                                      _("blend mode not available in the effective colorspace"));
      return FALSE;
    }
    dst->blend_mode = (dst->blend_mode & DEVELOP_BLEND_REVERSE) | mode_value;
  }

  // 4. reverse
  if(json_object_has_member(patch, "reverse"))
  {
    JsonNode *node = json_object_get_member(patch, "reverse");
    if(!node || !JSON_NODE_HOLDS_VALUE(node)
       || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "reverse", "wrong_type",
                                      _("'reverse' must be a boolean"));
      return FALSE;
    }
    if(json_node_get_boolean(node)) dst->blend_mode |= DEVELOP_BLEND_REVERSE;
    else dst->blend_mode &= ~DEVELOP_BLEND_REVERSE;
  }

  // 5. feathering_guide
  if(json_object_has_member(patch, "feathering_guide"))
  {
    uint32_t fg_value = 0;
    const char *fg_string = _json_member_string(patch, "feathering_guide");
    if(!fg_string || !_enum_value_for_name(_feathering_guide_names, fg_string, &fg_value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "feathering_guide",
                                      "unknown_value", _("unknown feathering guide"));
      return FALSE;
    }
    dst->feathering_guide = fg_value;
  }

  // 6. numeric fields, table order
  for(const _float_field_t *f = _float_fields; f->name; f++)
  {
    if(!json_object_has_member(patch, f->name)) continue;
    if(!strcmp(f->name, "details") && !_details_writable(module))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, "details",
                                      "requires_raw_image",
                                      _("the details threshold needs a raw image"));
      return FALSE;
    }
    double value = 0.0;
    if(!_json_member_double(patch, f->name, &value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "wrong_type",
                                      _("'%s' must be a number"), f->name);
      return FALSE;
    }
    if(!isfinite(value))
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "non_finite",
                                      _("'%s' must be finite"), f->name);
      return FALSE;
    }
    if(value < (double)f->min || value > (double)f->max)
    {
      if(error) *error = _blend_error(DT_REMOTE_ERR_INVALID_VALUE, f->name, "range",
                                      _("'%s' must be within [%g, %g]"), f->name,
                                      (double)f->min, (double)f->max);
      return FALSE;
    }
    *(float *)((guint8 *)dst + f->offset) = (float)value;
  }

  return TRUE;
}
