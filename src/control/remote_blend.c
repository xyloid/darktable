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

gboolean dt_remote_blend_mask_mode_from_string(const char *s, uint32_t *out)
{
  if(!s) return FALSE;
  if(!strcmp(s, "off")) { *out = DEVELOP_MASK_DISABLED; return TRUE; }
  if(!strcmp(s, "uniform")) { *out = DEVELOP_MASK_ENABLED; return TRUE; }
  return FALSE;
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
