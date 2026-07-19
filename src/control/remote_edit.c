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

#include "control/remote_edit.h"

#include "common/darktable.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/remote_band.h"
#include "control/remote_curve.h"
#include "control/remote_quantity.h"
#include "control/remote_revision.h"
#include "control/remote_vector.h"
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"
#include "views/view.h"

#include <pthread.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper                                                            */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_error_new(dt_remote_error_code_t code,
                                              const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_error_new(dt_remote_error_code_t code,
                                              const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = code;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  return error;
}

void dt_remote_error_free(dt_remote_error_t *error)
{
  if(!error) return;
  g_free(error->message);
  g_free(error->details_json);
  g_free(error);
}

/* ---------------------------------------------------------------------- */
/* free functions                                                          */
/* ---------------------------------------------------------------------- */

void dt_remote_value_clear(dt_remote_value_t *v)
{
  if(!v) return;
  if(v->type == DT_REMOTE_VALUE_ENUM)
  {
    g_free(v->v.e.name);
    v->v.e.name = NULL;
  }
}

void dt_remote_field_free(gpointer field_ptr)
{
  dt_remote_field_t *field = field_ptr;
  if(!field) return;
  dt_remote_value_clear(&field->default_value);
  if(field->enum_values) g_ptr_array_unref(field->enum_values);
  if(field->represented_by) g_ptr_array_unref(field->represented_by);
  g_free(field);
}

void dt_remote_module_free(gpointer module_ptr)
{
  dt_remote_module_t *module = module_ptr;
  if(!module) return;
  g_free(module->op);
  g_free(module->instance_name);
  g_free(module->display_name);
  g_free(module);
}

void dt_remote_module_schema_free(dt_remote_module_schema_t *schema)
{
  if(!schema) return;
  g_free(schema->op);
  g_free(schema->display_name);
  if(schema->fields) g_ptr_array_unref(schema->fields);
  if(schema->semantic_fields) g_ptr_array_unref(schema->semantic_fields);
  g_free(schema);
}

void dt_remote_state_free(dt_remote_state_t *state)
{
  if(!state) return;
  g_free(state->view);
  g_free(state->image_filename);
  g_free(state->maker);
  g_free(state->model);
  g_free(state->lens);
  g_free(state);
}

void dt_remote_history_item_free(gpointer item_ptr)
{
  dt_remote_history_item_t *item = item_ptr;
  if(!item) return;
  g_free(item->op);
  g_free(item->display_name);
  g_free(item->instance_name);
  g_free(item);
}

void dt_remote_patch_entry_free(gpointer entry_ptr)
{
  dt_remote_patch_entry_t *entry = entry_ptr;
  if(!entry) return;
  g_free(entry->name);
  dt_remote_value_clear(&entry->value);
  g_free(entry);
}

void dt_remote_mutation_result_free(dt_remote_mutation_result_t *result)
{
  if(!result) return;
  g_free(result->op);
  g_free(result->instance_name);
  if(result->values) g_ptr_array_unref(result->values);
  if(result->semantic_values) g_hash_table_unref(result->semantic_values);
  g_free(result);
}

/* ---------------------------------------------------------------------- */
/* pure conversion layer (2a)                                               */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_value_from_field(const dt_introspection_field_t *f,
                                    const void *params_blob,
                                    dt_remote_value_t *out)
{
  if(!f || !params_blob || !out) return FALSE;

  const uint8_t *base = (const uint8_t *)params_blob + f->header.offset;

  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      out->type = DT_REMOTE_VALUE_FLOAT;
      out->v.f = *(const float *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      out->type = DT_REMOTE_VALUE_FLOAT;
      out->v.f = *(const double *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_CHAR:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const char *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_INT8:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const int8_t *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_UINT8:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const uint8_t *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_SHORT:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const short *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_USHORT:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const unsigned short *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_INT:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const int *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_UINT:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const unsigned int *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_LONG:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = *(const long *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ULONG:
      out->type = DT_REMOTE_VALUE_INT;
      out->v.i = (int64_t)*(const unsigned long *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_BOOL:
      out->type = DT_REMOTE_VALUE_BOOL;
      out->v.b = *(const gboolean *)base;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const int value = *(const int *)base;
      const char *name =
        dt_introspection_get_enum_name((dt_introspection_field_t *)f, value);
      out->type = DT_REMOTE_VALUE_ENUM;
      out->v.e.value = value;
      out->v.e.name = g_strdup(name ? name : "");
      return TRUE;
    }
    default:
      // ARRAY / STRUCT / UNION / OPAQUE / FLOATCOMPLEX: not one of the
      // four v1 value classes.
      return FALSE;
  }
}

gboolean dt_remote_value_validate_and_write(const dt_introspection_field_t *f,
                                            const dt_remote_value_t *v,
                                            void *params_blob,
                                            dt_remote_error_t **err)
{
  if(!f || !v || !params_blob)
  {
    if(err) *err = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL, _("internal error: null argument"));
    return FALSE;
  }

  uint8_t *base = (uint8_t *)params_blob + f->header.offset;

  switch(f->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT:
      if(v->type != DT_REMOTE_VALUE_FLOAT) goto invalid_type;
      if(!(v->v.f >= f->Float.Min && v->v.f <= f->Float.Max)) goto out_of_range;
      *(float *)base = (float)v->v.f;
      return TRUE;
    case DT_INTROSPECTION_TYPE_DOUBLE:
      if(v->type != DT_REMOTE_VALUE_FLOAT) goto invalid_type;
      if(!(v->v.f >= f->Double.Min && v->v.f <= f->Double.Max)) goto out_of_range;
      *(double *)base = v->v.f;
      return TRUE;
    case DT_INTROSPECTION_TYPE_CHAR:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->Char.Min && v->v.i <= f->Char.Max)) goto out_of_range;
      *(char *)base = (char)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_INT8:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->Int8.Min && v->v.i <= f->Int8.Max)) goto out_of_range;
      *(int8_t *)base = (int8_t)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_UINT8:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->UInt8.Min && v->v.i <= f->UInt8.Max)) goto out_of_range;
      *(uint8_t *)base = (uint8_t)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_SHORT:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->Short.Min && v->v.i <= f->Short.Max)) goto out_of_range;
      *(short *)base = (short)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_USHORT:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->UShort.Min && v->v.i <= f->UShort.Max)) goto out_of_range;
      *(unsigned short *)base = (unsigned short)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_INT:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->Int.Min && v->v.i <= f->Int.Max)) goto out_of_range;
      *(int *)base = (int)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_UINT:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->UInt.Min && v->v.i <= f->UInt.Max)) goto out_of_range;
      *(unsigned int *)base = (unsigned int)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_LONG:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= f->Long.Min && v->v.i <= f->Long.Max)) goto out_of_range;
      *(long *)base = (long)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ULONG:
      if(v->type != DT_REMOTE_VALUE_INT) goto invalid_type;
      if(!(v->v.i >= 0 && (unsigned long)v->v.i <= f->ULong.Max)) goto out_of_range;
      *(unsigned long *)base = (unsigned long)v->v.i;
      return TRUE;
    case DT_INTROSPECTION_TYPE_BOOL:
      if(v->type != DT_REMOTE_VALUE_BOOL) goto invalid_type;
      *(gboolean *)base = v->v.b ? TRUE : FALSE;
      return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      if(v->type != DT_REMOTE_VALUE_ENUM) goto invalid_type;
      const char *name =
        dt_introspection_get_enum_name((dt_introspection_field_t *)f, v->v.e.value);
      if(!name)
      {
        if(err)
          *err = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                     _("value %d is not a member of enum field '%s'"),
                                     v->v.e.value, f->header.name);
        return FALSE;
      }
      *(int *)base = v->v.e.value;
      return TRUE;
    }
    default:
      // ARRAY / STRUCT / UNION / OPAQUE / FLOATCOMPLEX: not writable.
      if(err)
        *err = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                   _("field '%s' is not a writable scalar field"),
                                   f->header.name);
      return FALSE;
  }

invalid_type:
  if(err)
    *err = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("value type does not match field '%s'"),
                               f->header.name);
  return FALSE;

out_of_range:
  if(err)
    *err = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("value out of range for field '%s'"),
                               f->header.name);
  return FALSE;
}

gboolean dt_remote_denylisted(const dt_remote_denylist_t *denylist, const char *name)
{
  if(!denylist || !denylist->names || !name) return FALSE;
  for(const char *const *n = denylist->names; *n; n++)
    if(!g_strcmp0(*n, name)) return TRUE;
  return FALSE;
}

// Per-op forced-writable:false table. Source of truth:
// docs/superpowers/specs/2026-07-05-darktable-mcp-supported-operations.md
// (appendix); every entry verified against src/iop/ at commit time.
typedef struct dt_remote_op_denylist_t
{
  const char *op;
  dt_remote_denylist_t denylist;
} dt_remote_op_denylist_t;

#define DENY(...) { .names = (const char *const[]){ __VA_ARGS__, NULL } }

static const dt_remote_op_denylist_t s_op_denylists[] = {
  { "ashift",          DENY("cl", "cr", "ct", "cb",
                            "last_drawn_lines_count", "last_quad_lines") },
  { "atrous",          DENY("octaves") },
  { "channelmixerrgb", DENY("x", "y", "version") },
  { "colorcontrast",   DENY("a_offset", "b_offset", "unbound") },
  { "colorize",        DENY("version") },
  { "colorzones",      DENY("splines_version") },
  { "crop",            DENY("ratio_n", "ratio_d") },
  { "denoiseprofile",  DENY("fix_anscombe_and_nlmeans_norm", "use_new_vst",
                            "wb_adaptive_anscombe") },
  { "dither",          DENY("palette", "random.radius", "random.range") },
  { "filmicrgb",       DENY("version", "spline_version") },
  { "highlights",      DENY("blendL", "blendC") },
  { "lens",            DENY("crop", "focal", "aperture", "distance",
                            "has_been_set", "md_version", "reserved") },
  { "lowpass",         DENY("unbound") },
  { "overlay",         DENY("imgid", "dummy0", "dummy1", "dummy2") },
  { "relight",         DENY("center") },
  { "shadhi",          DENY("reserved2", "flags", "low_approximation") },
  { "temperature",     DENY("preset") },
  { "tonecurve",       DENY("tonecurve_preset", "tonecurve_unbound_ab") },
  { "vignette",        DENY("unbound") },
};

const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op)
{
  if(!op) return NULL;
  for(size_t i = 0; i < G_N_ELEMENTS(s_op_denylists); i++)
    if(!strcmp(s_op_denylists[i].op, op)) return &s_op_denylists[i].denylist;
  return NULL;
}

GPtrArray *dt_remote_schema_from_introspection(const dt_introspection_field_t *linear,
                                               const dt_remote_denylist_t *denylist)
{
  GPtrArray *fields = g_ptr_array_new_with_free_func(dt_remote_field_free);
  if(!linear) return fields;

  for(const dt_introspection_field_t *f = linear;
      f->header.type != DT_INTROSPECTION_TYPE_NONE;
      f++)
  {
    dt_remote_field_t *rf = g_malloc0(sizeof(dt_remote_field_t));
    rf->name = (char *)(f->header.name ? f->header.name : "");
    rf->description = (char *)(f->header.description ? f->header.description : "");

    gboolean supported = TRUE;

    switch(f->header.type)
    {
      case DT_INTROSPECTION_TYPE_FLOAT:
        rf->type_name = "float";
        rf->has_range = TRUE;
        rf->minimum = f->Float.Min;
        rf->maximum = f->Float.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_FLOAT;
        rf->default_value.v.f = f->Float.Default;
        break;
      case DT_INTROSPECTION_TYPE_DOUBLE:
        rf->type_name = "float";
        rf->has_range = TRUE;
        rf->minimum = f->Double.Min;
        rf->maximum = f->Double.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_FLOAT;
        rf->default_value.v.f = f->Double.Default;
        break;
      case DT_INTROSPECTION_TYPE_CHAR:
        rf->type_name = "int";
        rf->has_range = TRUE;
        rf->minimum = f->Char.Min;
        rf->maximum = f->Char.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->Char.Default;
        break;
      case DT_INTROSPECTION_TYPE_INT8:
        rf->type_name = "int";
        rf->has_range = TRUE;
        rf->minimum = f->Int8.Min;
        rf->maximum = f->Int8.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->Int8.Default;
        break;
      case DT_INTROSPECTION_TYPE_UINT8:
        rf->type_name = "uint";
        rf->has_range = TRUE;
        rf->minimum = f->UInt8.Min;
        rf->maximum = f->UInt8.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->UInt8.Default;
        break;
      case DT_INTROSPECTION_TYPE_SHORT:
        rf->type_name = "int";
        rf->has_range = TRUE;
        rf->minimum = f->Short.Min;
        rf->maximum = f->Short.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->Short.Default;
        break;
      case DT_INTROSPECTION_TYPE_USHORT:
        rf->type_name = "uint";
        rf->has_range = TRUE;
        rf->minimum = f->UShort.Min;
        rf->maximum = f->UShort.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->UShort.Default;
        break;
      case DT_INTROSPECTION_TYPE_INT:
        rf->type_name = "int";
        rf->has_range = TRUE;
        rf->minimum = f->Int.Min;
        rf->maximum = f->Int.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->Int.Default;
        break;
      case DT_INTROSPECTION_TYPE_UINT:
        rf->type_name = "uint";
        rf->has_range = TRUE;
        rf->minimum = f->UInt.Min;
        rf->maximum = f->UInt.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->UInt.Default;
        break;
      case DT_INTROSPECTION_TYPE_LONG:
        rf->type_name = "int";
        rf->has_range = TRUE;
        rf->minimum = (double)f->Long.Min;
        rf->maximum = (double)f->Long.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = f->Long.Default;
        break;
      case DT_INTROSPECTION_TYPE_ULONG:
        rf->type_name = "uint";
        rf->has_range = TRUE;
        rf->minimum = (double)f->ULong.Min;
        rf->maximum = (double)f->ULong.Max;
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_INT;
        rf->default_value.v.i = (int64_t)f->ULong.Default;
        break;
      case DT_INTROSPECTION_TYPE_BOOL:
        rf->type_name = "bool";
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_BOOL;
        rf->default_value.v.b = f->Bool.Default;
        break;
      case DT_INTROSPECTION_TYPE_ENUM:
      {
        rf->type_name = "enum";
        rf->has_default = TRUE;
        rf->default_value.type = DT_REMOTE_VALUE_ENUM;
        rf->default_value.v.e.value = f->Enum.Default;
        const char *dname =
          dt_introspection_get_enum_name((dt_introspection_field_t *)f, f->Enum.Default);
        rf->default_value.v.e.name = g_strdup(dname ? dname : "");

        rf->enum_values = g_ptr_array_new_with_free_func(g_free);
        for(const dt_introspection_type_enum_tuple_t *e = f->Enum.values; e && e->name; e++)
        {
          dt_remote_enum_value_t *ev = g_malloc0(sizeof(dt_remote_enum_value_t));
          ev->name = (char *)e->name;
          ev->value = e->value;
          ev->description = (char *)(e->description ? e->description : "");
          g_ptr_array_add(rf->enum_values, ev);
        }
        break;
      }
      case DT_INTROSPECTION_TYPE_ARRAY:
        rf->type_name = (f->Array.type == DT_INTROSPECTION_TYPE_CHAR) ? "string" : "array";
        supported = FALSE;
        break;
      case DT_INTROSPECTION_TYPE_STRUCT:
        rf->type_name = "struct";
        supported = FALSE;
        break;
      case DT_INTROSPECTION_TYPE_UNION:
        rf->type_name = "struct";
        supported = FALSE;
        break;
      case DT_INTROSPECTION_TYPE_OPAQUE:
        rf->type_name = "opaque";
        supported = FALSE;
        break;
      case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
        rf->type_name = "opaque";
        supported = FALSE;
        break;
      default:
        rf->type_name = "opaque";
        supported = FALSE;
        break;
    }

    rf->writable = supported && !dt_remote_denylisted(denylist, rf->name);

    g_ptr_array_add(fields, rf);
  }

  return fields;
}

static const dt_introspection_field_t *dt_remote_find_field(const dt_introspection_field_t *linear,
                                                             const char *name)
{
  if(!linear || !name) return NULL;
  for(const dt_introspection_field_t *f = linear; f->header.type != DT_INTROSPECTION_TYPE_NONE; f++)
    if(f->header.name && !strcmp(f->header.name, name)) return f;
  return NULL;
}

gboolean dt_remote_patch_apply(const dt_introspection_field_t *linear,
                               const dt_remote_denylist_t *denylist,
                               const dt_remote_patch_t *patch,
                               void *params_blob,
                               dt_remote_error_t **error)
{
  const gboolean has_scalars = patch && patch->scalar_values && patch->scalar_values->len > 0;
  const gboolean has_semantics = patch && patch->semantic_values && patch->semantic_values->len > 0;

  if(!patch || (!has_scalars && !has_semantics && !patch->has_enable))
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("values must be non-empty"));
    return FALSE;
  }

  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *entry = g_ptr_array_index(patch->scalar_values, i);

    if(!entry->name || !*entry->name)
    {
      if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_FIELD, _("empty field name"));
      return FALSE;
    }

    // duplicate-within-this-patch check (belt-and-suspenders: the JSON
    // protocol layer cannot itself produce this -- object keys are
    // already unique by the time they reach here -- but this pure core
    // does not assume a JSON-shaped caller; see the header comment).
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_patch_entry_t *prior = g_ptr_array_index(patch->scalar_values, j);
      if(!g_strcmp0(prior->name, entry->name))
      {
        if(error)
          *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                       _("duplicate field '%s' in patch"), entry->name);
        return FALSE;
      }
    }

    const dt_introspection_field_t *f = dt_remote_find_field(linear, entry->name);
    if(!f)
    {
      if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_FIELD, _("unknown field '%s'"), entry->name);
      return FALSE;
    }

    if(dt_remote_denylisted(denylist, entry->name))
    {
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD,
                                     _("field '%s' is not writable"), entry->name);
      return FALSE;
    }

    if(!dt_remote_value_validate_and_write(f, &entry->value, params_blob, error))
      return FALSE;
  }

  // Scalar-only completed-state validation is a no-op: every field is
  // already individually validated above. Semantic completed-state
  // validation is composed by dt_remote_curve_apply_patch() in the live
  // transaction after this helper returns.
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* live-module traversal (read API)                                        */
/* ---------------------------------------------------------------------- */

// Precondition for the read API calls that inspect the live darkroom
// image (list_modules, get_module_params): the sidecar can only walk
// module instances of the image currently loaded in the darkroom
// (internals doc §1, plan step-1 implementation requirements). get_state
// and get_module_schema do not gate on this -- see their own comments.
// `dev_out`, if non-NULL, receives darktable.develop on success.
static gboolean dt_remote_require_darkroom_image(dt_develop_t **dev_out, dt_remote_error_t **error)
{
  if(dt_view_get_current() != DT_VIEW_DARKROOM)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_NOT_IN_DARKROOM,
                                   _("no darkroom view is active"));
    return FALSE;
  }

  dt_develop_t *dev = darktable.develop;
  if(!dev || !dt_is_valid_imgid(dev->image_storage.id))
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_NO_IMAGE_OPEN,
                                   _("no image is open in the darkroom"));
    return FALSE;
  }

  if(dev_out) *dev_out = dev;
  return TRUE;
}

// stable, untranslated identifier for the wire `view` field -- the view's
// own module_name (e.g. "darkroom", "lighttable"), never a display string.
static const char *dt_remote_current_view_name(void)
{
  const dt_view_t *cv = darktable.view_manager
    ? dt_view_manager_get_current_view(darktable.view_manager)
    : NULL;
  return (cv && *cv->module_name) ? cv->module_name : "none";
}

// get_state has no domain errors in the protocol reference's error matrix:
// it always succeeds, reporting has_image:FALSE (and zeroed/NULL image
// fields) when not in darkroom or no image is open, rather than gating on
// dt_remote_require_darkroom_image() like the module-inspection calls do.
gboolean dt_remote_get_state(dt_remote_state_t **out, dt_remote_error_t **error)
{
  dt_remote_state_t *state = g_malloc0(sizeof(dt_remote_state_t));
  state->view = g_strdup(dt_remote_current_view_name());

  dt_develop_t *dev = darktable.develop;
  if(dt_view_get_current() == DT_VIEW_DARKROOM && dev && dt_is_valid_imgid(dev->image_storage.id))
  {
    state->has_image = TRUE;
    state->image_id = dev->image_storage.id;
    state->image_filename = g_strdup(dev->image_storage.filename);
    state->width = dev->image_storage.width;
    state->height = dev->image_storage.height;
    state->maker = g_strdup(dev->image_storage.exif_maker);
    state->model = g_strdup(dev->image_storage.exif_model);
    state->lens = g_strdup(dev->image_storage.exif_lens);
    state->iso = dev->image_storage.exif_iso;
    state->aperture = dev->image_storage.exif_aperture;
    state->exposure_time = dev->image_storage.exif_exposure;
    state->focal_length = dev->image_storage.exif_focal_length;
    // Real process-local revision tracker (internals doc §5): NOT the
    // database's history_end (that survives restarts and is not
    // monotonic across undo -- see dt_remote_revision_t's own header
    // comment). Self-healing read (fix round 2): if the tracker never
    // learned the current darkroom image (its DEVELOP_IMAGE_CHANGED can
    // be raised before dt_control_running() during startup and silently
    // dropped), dt_remote_revision_observe_image() stamps it now -- so
    // the revision handed out here always CAS-matches in a subsequent
    // set_module_params against unchanged state, instead of the client
    // conflicting forever against an imgid of NO_IMGID. In production
    // this only runs while a server is connected; a unit-test call with
    // a valid fixture image observes the test singleton, same as the
    // simulation seams do.
    state->revision = dt_remote_revision_observe_image(dev->image_storage.id);
  }
  // else: has_image stays FALSE and every image_* field stays at its
  // g_malloc0() zero/NULL value -- no error, per the protocol reference.

  *out = state;
  return TRUE;
}

// module->multi_name[0] is empty (or the legacy "0") for the default,
// unnamed instance -- see dt_iop_gui_update_header/dt_iop_update_multi_name.
static gboolean dt_remote_instance_name_is_default(const dt_iop_module_t *module)
{
  return module->multi_name[0] == '\0' || !strcmp(module->multi_name, "0");
}

gboolean dt_remote_list_modules(GPtrArray **out, dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  GPtrArray *modules = g_ptr_array_new_with_free_func(dt_remote_module_free);

  for(GList *m = dev->iop; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = m->data;

    dt_remote_module_t *entry = g_malloc0(sizeof(dt_remote_module_t));
    entry->op = g_strdup(module->op);
    entry->instance = module->multi_priority;
    entry->instance_name =
      dt_remote_instance_name_is_default(module) ? g_strdup("") : g_strdup(module->multi_name);
    entry->display_name = g_strdup(module->name());
    entry->enabled = module->enabled;
    entry->deprecated = (module->flags() & IOP_FLAGS_DEPRECATED) != 0;
    entry->supports_multiple_instances = !(module->flags() & IOP_FLAGS_ONE_INSTANCE);

    g_ptr_array_add(modules, entry);
  }

  *out = modules;
  return TRUE;
}

// Schemas are derived from the loaded module .so alone (op name, flags,
// introspection descriptor) -- they are per-op and cacheable per
// (darktable version, op), independent of any open image or active view,
// so unlike list_modules/get_module_params this does not gate on
// dt_remote_require_darkroom_image(). The protocol reference's normal error
// matrix lists unknown_module/invalid_value here; semantic registry drift
// additionally propagates DT_REMOTE_ERR_INTERNAL from the full schema path.
gboolean dt_remote_get_module_primitive_schema(const char *op,
                                               dt_remote_module_schema_t **out,
                                               dt_remote_error_t **error)
{
  if(!op || !*op)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_MODULE, _("no operation given"));
    return FALSE;
  }

  dt_iop_module_so_t *so = dt_iop_get_module_so(op);
  if(!so)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_MODULE, _("unknown module '%s'"), op);
    return FALSE;
  }

  dt_introspection_field_t *linear = so->get_introspection_linear();
  dt_introspection_t *intro = so->get_introspection();

  dt_remote_module_schema_t *schema = g_malloc0(sizeof(dt_remote_module_schema_t));
  schema->op = g_strdup(op);
  schema->display_name = g_strdup(so->name());
  schema->params_version = intro ? intro->params_version : 0;
  schema->deprecated = (so->flags() & IOP_FLAGS_DEPRECATED) != 0;
  schema->supports_multiple_instances = !(so->flags() & IOP_FLAGS_ONE_INSTANCE);
  schema->fields = dt_remote_schema_from_introspection(linear, dt_remote_denylist_for_op(op));

  *out = schema;
  return TRUE;
}

// Stamps `semantic_name` onto the primitive field at `path`'s root segment
// -- the wire schema's back-reference from a native storage field to the
// semantic parameter(s) that own it (curve design SS Schema response).
// `path` must start at a field (curve node/count/type paths and vector
// native paths all do, by construction); a path that does not, or a root
// name no schema row carries, is registry/introspection drift and fails
// closed like every other registry inconsistency. One root often backs
// several descriptors (rgbcurve's shared channel arrays, colorbalance/
// rgblevels aliasing) and, for curves, several paths of one descriptor --
// dedupe so a name is never added twice to the same field.
// `class_name` is used to select class-specific error wording ("curve",
// "vector", "band", or "quantity") to preserve original error message text
// per-class.
static gboolean _stamp_root(dt_remote_module_schema_t *schema,
                            const dt_remote_introspection_path_t *path,
                            const char *semantic_name,
                            const char *class_name,
                            dt_remote_error_t **error)
{
  if(path->length == 0 || path->segments[0].type != DT_REMOTE_PATH_FIELD)
  {
    if(error)
    {
      if(!g_strcmp0(class_name, "curve"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("curve descriptor '%s' has a rootless native path"), semantic_name);
      else if(!g_strcmp0(class_name, "band"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("band descriptor '%s' has a rootless native path"), semantic_name);
      else if(!g_strcmp0(class_name, "quantity"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("quantity descriptor '%s' has a rootless native path"), semantic_name);
      else
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("vector descriptor '%s' has a rootless native path"), semantic_name);
    }
    return FALSE;
  }
  const char *root = path->segments[0].value.field;

  dt_remote_field_t *field = NULL;
  for(guint i = 0; schema->fields && i < schema->fields->len; i++)
  {
    dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
    if(!g_strcmp0(f->name, root)) { field = f; break; }
  }
  if(!field)
  {
    if(error)
    {
      if(!g_strcmp0(class_name, "curve"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("curve descriptor '%s' routes through unknown field '%s'"),
                                     semantic_name, root);
      else if(!g_strcmp0(class_name, "band"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("band descriptor '%s' routes through unknown field '%s'"),
                                     semantic_name, root);
      else if(!g_strcmp0(class_name, "quantity"))
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("quantity descriptor '%s' routes through unknown field '%s'"),
                                     semantic_name, root);
      else
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("vector descriptor '%s' routes through unknown field '%s'"),
                                     semantic_name, root);
    }
    return FALSE;
  }

  if(!field->represented_by) field->represented_by = g_ptr_array_new_with_free_func(g_free);
  gboolean present = FALSE;
  for(guint i = 0; !present && i < field->represented_by->len; i++)
    present = !g_strcmp0(g_ptr_array_index(field->represented_by, i), semantic_name);
  if(!present) g_ptr_array_add(field->represented_by, g_strdup(semantic_name));
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* class-ops dispatch table                                                */
/* ---------------------------------------------------------------------- */

// One row per semantic parameter class, gathering each class engine's
// schema/read/apply entry points plus the wrapper that tags its plain
// class-specific type with dt_remote_parameter_class_t for the neutral
// semantic_fields/semantic_values containers (remote_parameters.h). The four
// seams below (schema listing, readback, apply, represented_by annotation)
// each loop over this table once instead of making one explicit call per
// class -- milestone 5's bands class is exactly the third row this table
// was built for, added without touching any seam's loop body.
typedef struct dt_remote_class_ops_t
{
  dt_remote_parameter_class_t class_id;
  gboolean (*list_schema)(const struct dt_iop_module_so_t *so,
                          GPtrArray **out_fields, dt_remote_error_t **error);
  gboolean (*read_values)(const struct dt_iop_module_t *module,
                          const void *params, GHashTable **out,
                          dt_remote_error_t **error);
  // Takes the FULL patch, not a pre-partitioned slice: each engine's own
  // apply_patch() wrapper (remote_curve.h/remote_vector.h) partitions its
  // own class's entries out of `patch->semantic_values` internally, and --
  // critically -- computes its own prepare_needed gate from
  // `patch->scalar_values` before ever looking at semantic entries, so a
  // scalar-only patch that mentions a prepare_field still reaches that
  // engine's prepare()/validate_completed() even when the patch carries
  // zero entries of that engine's class (see e.g. rgbcurve's
  // compensate_middle_grey / colorzones' channel). A dispatcher that only
  // called an engine when its own semantic slice was non-empty would drop
  // that behavior -- proven by
  // test_transaction_middle_grey_without_work_profile_fails_without_mutation
  // (test_remote_edit.c) and
  // test_rgbcurve_adapter_foreign_class_entry_with_colliding_name_is_ignored
  // (test_remote_curve.c) -- so every row is called unconditionally, in
  // table order, exactly as the pre-refactor code called both engines
  // unconditionally.
  gboolean (*apply_patch)(const struct dt_iop_module_t *module,
                          const void *old_params, void *new_params,
                          const dt_remote_patch_t *patch,
                          dt_remote_error_t **error);
  dt_remote_semantic_schema_t *(*wrap_schema)(gpointer class_schema);
  dt_remote_semantic_value_t *(*wrap_value)(gpointer class_value);
  // Stamps this class's represented_by back-references onto `schema`'s
  // primitive fields (see _stamp_root/_annotate_represented_by below) --
  // kept as a per-row lookup+stamp pointer because the classes' native
  // layouts genuinely differ (a curve stamps three native paths per
  // descriptor -- nodes/count/type -- a vector stamps one, a band stamps
  // two -- x/y), so unlike the four members above there is no single shared
  // loop body to drive from a uniform per-descriptor shape.
  gboolean (*annotate_represented_by)(dt_remote_module_schema_t *schema,
                                      dt_introspection_t *intro,
                                      guint params_version,
                                      dt_remote_error_t **error);
} dt_remote_class_ops_t;

// Runs only when the schema listing seam already advertised at least one
// curve field, so the curve adapter has already passed registry validation
// -- an advertised curve class whose adapter lookup here comes back empty is
// registry/introspection drift and fails closed, same as every other
// registry inconsistency (see _stamp_root's own header comment).
static gboolean _annotate_curve_represented_by(dt_remote_module_schema_t *schema,
                                               dt_introspection_t *intro,
                                               guint params_version,
                                               dt_remote_error_t **error)
{
  const dt_remote_curve_module_adapter_t *adapter =
    intro ? dt_remote_curve_registry_lookup(schema->op, params_version) : NULL;
  if(!adapter)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   _("semantic curves advertised without a registry adapter for '%s'"),
                                   schema->op);
    return FALSE;
  }

  for(guint c = 0; c < adapter->curve_count; c++)
  {
    const dt_remote_curve_descriptor_t *desc = &adapter->curves[c];
    const dt_remote_introspection_path_t *paths[] = { &desc->native.nodes, &desc->native.count,
                                                      &desc->native.type };
    for(guint p = 0; p < G_N_ELEMENTS(paths); p++)
      if(!_stamp_root(schema, paths[p], desc->name, "curve", error)) return FALSE;
  }
  return TRUE;
}

// Same rationale as _annotate_curve_represented_by above, for the vector
// class -- one native path per descriptor rather than three.
static gboolean _annotate_vector_represented_by(dt_remote_module_schema_t *schema,
                                                dt_introspection_t *intro,
                                                guint params_version,
                                                dt_remote_error_t **error)
{
  const dt_remote_vector_module_adapter_t *adapter =
    intro ? dt_remote_vector_registry_lookup(schema->op, params_version) : NULL;
  if(!adapter)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   _("semantic vectors advertised without a registry adapter for '%s'"),
                                   schema->op);
    return FALSE;
  }

  for(guint v = 0; v < adapter->vector_count; v++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[v];
    if(!_stamp_root(schema, &desc->native, desc->name, "vector", error)) return FALSE;
  }
  return TRUE;
}

// Same rationale again for the band class -- two native paths per
// descriptor (native_x and native_y), both stamped with the one semantic
// name.
static gboolean _annotate_band_represented_by(dt_remote_module_schema_t *schema,
                                              dt_introspection_t *intro,
                                              guint params_version,
                                              dt_remote_error_t **error)
{
  const dt_remote_band_module_adapter_t *adapter =
    intro ? dt_remote_band_registry_lookup(schema->op, params_version) : NULL;
  if(!adapter)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   _("semantic bands advertised without a registry adapter for '%s'"),
                                   schema->op);
    return FALSE;
  }

  for(guint band = 0; band < adapter->band_count; band++)
  {
    const dt_remote_band_descriptor_t *desc = &adapter->bands[band];
    const dt_remote_introspection_path_t *paths[] = { &desc->native_x, &desc->native_y };
    for(guint p = 0; p < G_N_ELEMENTS(paths); p++)
      if(!_stamp_root(schema, paths[p], desc->name, "band", error)) return FALSE;
  }
  return TRUE;
}

// Unlike the curve/vector/band classes above, a quantity descriptor has no
// native storage path of its own (remote_quantity.h's own top comment): its
// module adapter's `native_fields` (the ordinary scalar params fields the
// conversion write hook may touch) are the closest analogue. Stamp every
// declared native field, for every descriptor on the adapter (an adapter's
// `native_fields` list is shared across all of its descriptors, since which
// descriptor's conversion touches which field isn't statically known at
// this layer), with that descriptor's semantic name -- _stamp_root leaves
// the primitive field's `writable` flag untouched, so a stamped native
// field keeps `writable: true` while also advertising the semantic
// parameter(s) that can write it via conversion. This is the "coefficient
// coexistence" exception's discoverability half (see the milestone-6
// design doc's SS Coefficient coexistence, and remote_quantity.h's top
// comment).
static gboolean _annotate_quantity_represented_by(dt_remote_module_schema_t *schema,
                                                  dt_introspection_t *intro,
                                                  guint params_version,
                                                  dt_remote_error_t **error)
{
  const dt_remote_quantity_module_adapter_t *adapter =
    intro ? dt_remote_quantity_registry_lookup(schema->op, params_version) : NULL;
  if(!adapter)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   _("semantic quantities advertised without a registry adapter for '%s'"),
                                   schema->op);
    return FALSE;
  }

  for(guint q = 0; q < adapter->quantity_count; q++)
  {
    const dt_remote_quantity_descriptor_t *desc = &adapter->quantities[q];
    for(guint n = 0; n < adapter->native_field_count; n++)
    {
      const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD,
                                                  .value.field = adapter->native_fields[n] };
      const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
      if(!_stamp_root(schema, &path, desc->name, "quantity", error)) return FALSE;
    }
  }
  return TRUE;
}

// Row order (curve, vector, bands, quantity) preserves today's error precedence and
// schema/readback insertion order -- both seams below process the table
// front to back. `wrap_schema`/`wrap_value` are cast from their concrete
// class-typed signatures (e.g. dt_remote_curve_schema_t *) to this table's
// gpointer-typed field: every producer here already deep-copies or
// transfers ownership through that single pointer argument, so the cast is
// a signature formality, not a layout assumption.
static const dt_remote_class_ops_t s_class_ops[] = {
  {
    .class_id = DT_REMOTE_PARAMETER_CURVE,
    .list_schema = dt_remote_curve_list_schema,
    .read_values = dt_remote_curve_read_values,
    .apply_patch = dt_remote_curve_apply_patch,
    .wrap_schema = (dt_remote_semantic_schema_t *(*)(gpointer))dt_remote_semantic_schema_wrap_curve,
    .wrap_value = (dt_remote_semantic_value_t *(*)(gpointer))dt_remote_semantic_value_wrap_curve,
    .annotate_represented_by = _annotate_curve_represented_by,
  },
  {
    .class_id = DT_REMOTE_PARAMETER_VECTOR,
    .list_schema = dt_remote_vector_list_schema,
    .read_values = dt_remote_vector_read_values,
    .apply_patch = dt_remote_vector_apply_patch,
    .wrap_schema = (dt_remote_semantic_schema_t *(*)(gpointer))dt_remote_semantic_schema_wrap_vector,
    .wrap_value = (dt_remote_semantic_value_t *(*)(gpointer))dt_remote_semantic_value_wrap_vector,
    .annotate_represented_by = _annotate_vector_represented_by,
  },
  {
    .class_id = DT_REMOTE_PARAMETER_BANDS,
    .list_schema = dt_remote_band_list_schema,
    .read_values = dt_remote_band_read_values,
    .apply_patch = dt_remote_band_apply_patch,
    .wrap_schema = (dt_remote_semantic_schema_t *(*)(gpointer))dt_remote_semantic_schema_wrap_band,
    .wrap_value = (dt_remote_semantic_value_t *(*)(gpointer))dt_remote_semantic_value_wrap_band,
    .annotate_represented_by = _annotate_band_represented_by,
  },
  {
    .class_id = DT_REMOTE_PARAMETER_QUANTITY,
    .list_schema = dt_remote_quantity_list_schema,
    .read_values = dt_remote_quantity_read_values,
    .apply_patch = dt_remote_quantity_apply_patch,
    .wrap_schema = (dt_remote_semantic_schema_t *(*)(gpointer))dt_remote_semantic_schema_wrap_quantity,
    .wrap_value = (dt_remote_semantic_value_t *(*)(gpointer))dt_remote_semantic_value_wrap_quantity,
    .annotate_represented_by = _annotate_quantity_represented_by,
  },
};

// Stamps represented_by onto the primitive fields for every semantic
// descriptor advertised in `schema->semantic_fields`: curve descriptors
// stamp their three native paths (nodes/count/type), vector descriptors
// stamp their single native path, in registry order within each class.
// Runs only after the schema listing seam advertised at least one semantic
// field of a class, so that class's adapter has already passed registry
// validation -- a class with advertised fields whose adapter lookup here
// comes back empty is registry/introspection drift and fails closed, same
// as every other registry inconsistency (see _stamp_root above).
static gboolean _annotate_represented_by(dt_remote_module_schema_t *schema,
                                         const dt_iop_module_so_t *so,
                                         dt_remote_error_t **error)
{
  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  const guint params_version = intro ? (guint)intro->params_version : 0;

  // One pass over the advertised semantic fields to learn which table rows
  // are present, then process rows in table order (curve, vector, bands) --
  // same two-phase shape the original has_curve/has_vector version used.
  gboolean row_present[G_N_ELEMENTS(s_class_ops)] = { FALSE };
  for(guint i = 0; schema->semantic_fields && i < schema->semantic_fields->len; i++)
  {
    const dt_remote_semantic_schema_t *wrapped = g_ptr_array_index(schema->semantic_fields, i);
    for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
      if(s_class_ops[row].class_id == wrapped->class_id) row_present[row] = TRUE;
  }

  for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
    if(row_present[row]
       && !s_class_ops[row].annotate_represented_by(schema, intro, params_version, error))
      return FALSE;

  return TRUE;
}

// The curve and vector engines (remote_curve.h, remote_vector.h) keep
// producing plain per-class schema/value containers; the helpers below
// convert their output into the class-tagged wrapper shape the
// semantic_fields/semantic_values members carry (remote_parameters.h),
// driven by the calling table row's own wrap_schema/wrap_value pointer
// instead of one function per class. Both consume their input container:
// element ownership moves into the wrappers (or, for the value merge, into
// `dst`).

// Wraps every element of `raw` (a class-specific schema GPtrArray -- may be
// NULL, since a class engine may silent-degrade to NULL rather than an
// empty array when an op has no adapter for it) and appends the wrapped
// results to `dst` (ownership transfers); consumes and releases `raw`.
static void _wrap_and_move_schemas(GPtrArray *dst, GPtrArray *raw,
                                   dt_remote_semantic_schema_t *(*wrap)(gpointer))
{
  if(!raw) return;
  for(guint i = 0; i < raw->len; i++)
    g_ptr_array_add(dst, wrap(g_ptr_array_index(raw, i)));
  g_ptr_array_set_free_func(raw, NULL);  // elements now owned by the wrappers
  g_ptr_array_unref(raw);
}

// Wraps every value of `raw` (a class-specific value GHashTable) and merges
// the wrapped results into `dst`, which may already hold wrapped entries of
// another class -- registry validation guarantees semantic IDs are unique
// across classes for one module, so no key can collide; consumes and
// releases `raw`.
static void _wrap_and_merge_values(GHashTable *dst, GHashTable *raw,
                                   dt_remote_semantic_value_t *(*wrap)(gpointer))
{
  if(!raw) return;
  GHashTableIter it;
  gpointer key = NULL;
  gpointer value = NULL;
  g_hash_table_iter_init(&it, raw);
  while(g_hash_table_iter_next(&it, &key, &value))
  {
    g_hash_table_iter_steal(&it);  // key and value ownership move below
    g_hash_table_insert(dst, key, wrap(value));
  }
  g_hash_table_unref(raw);
}

gboolean dt_remote_get_module_schema(const char *op,
                                     dt_remote_module_schema_t **out,
                                     dt_remote_error_t **error)
{
  dt_remote_module_schema_t *schema = NULL;
  if(!dt_remote_get_module_primitive_schema(op, &schema, error)) return FALSE;
  dt_iop_module_so_t *so = dt_iop_get_module_so(op);

  // One list_schema call per table row, in row order (curve, then vector --
  // the same deterministic aggregation order this seam always had). A row's
  // failure discards every earlier row's result and the primitive schema
  // before propagating that row's error; later rows are never reached.
  GPtrArray *raw_fields[G_N_ELEMENTS(s_class_ops)] = { NULL };
  guint total = 0;
  for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
  {
    dt_remote_error_t *row_error = NULL;
    if(!s_class_ops[row].list_schema(so, &raw_fields[row], &row_error))
    {
      for(gsize prev = 0; prev < row; prev++)
        if(raw_fields[prev]) g_ptr_array_unref(raw_fields[prev]);
      dt_remote_module_schema_free(schema);
      if(error)
        *error = row_error;
      else
        dt_remote_error_free(row_error);
      return FALSE;
    }
    total += raw_fields[row] ? raw_fields[row]->len : 0;
  }

  if(total > 0)
  {
    schema->semantic_fields = g_ptr_array_new_full(total, dt_remote_semantic_schema_free);
    for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
      _wrap_and_move_schemas(schema->semantic_fields, raw_fields[row], s_class_ops[row].wrap_schema);

    if(!_annotate_represented_by(schema, so, error))
    {
      dt_remote_module_schema_free(schema);
      return FALSE;
    }
  }
  else
  {
    for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
      if(raw_fields[row]) g_ptr_array_unref(raw_fields[row]);
  }

  *out = schema;
  return TRUE;
}

// Reads and wraps every table row's semantic values for `module`/`params`,
// merging them into one class-tagged GHashTable in table order (curve, then
// vector -- the same deterministic aggregation order the schema-listing
// seam above uses). A row's failure discards every earlier row's wrapped
// result before propagating that row's error; later rows are never
// reached. Shared by the two readback call sites below: the full snapshot
// in dt_remote_get_module_params() and the post-apply readback in
// dt_remote_set_module_params().
static gboolean _read_semantic_values(const dt_iop_module_t *module, const void *params,
                                      GHashTable **out, dt_remote_error_t **error)
{
  GHashTable *wrapped =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, dt_remote_semantic_value_free);
  for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
  {
    GHashTable *raw = NULL;
    if(!s_class_ops[row].read_values(module, params, &raw, error))
    {
      g_hash_table_unref(wrapped);
      return FALSE;
    }
    _wrap_and_merge_values(wrapped, raw, s_class_ops[row].wrap_value);
  }
  *out = wrapped;
  return TRUE;
}

// Shared by dt_remote_get_module_params() and dt_remote_set_module_params():
// resolves a module reference against dev->iop, distinguishing "no such
// op at all" (DT_REMOTE_ERR_UNKNOWN_MODULE) from "op known, but not this
// instance" (DT_REMOTE_ERR_UNKNOWN_INSTANCE) via the -1 "any instance"
// lookup, per the wire contract's error matrix (internals doc: "ref may
// address a module whose op exists but isn't in dev->iop -- unknown_module
// /unknown_instance semantics").
static dt_iop_module_t *dt_remote_find_module(dt_develop_t *dev, const dt_remote_module_ref_t *ref,
                                              dt_remote_error_t **error)
{
  if(!ref || !ref->op || !*ref->op)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_MODULE, _("no module reference given"));
    return NULL;
  }

  dt_iop_module_t *module = dt_iop_get_module_by_op_priority(dev->iop, ref->op, ref->instance);
  if(!module)
  {
    // -1 means "match any instance" -- distinguishes an unknown op from a
    // known op with no matching instance.
    dt_iop_module_t *any = dt_iop_get_module_by_op_priority(dev->iop, ref->op, -1);
    if(!any)
    {
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_MODULE, _("unknown module '%s'"), ref->op);
    }
    else
    {
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_UNKNOWN_INSTANCE,
                                     _("module '%s' has no instance %d"), ref->op, ref->instance);
    }
    return NULL;
  }

  return module;
}

gboolean dt_remote_get_module_params(const dt_remote_module_ref_t *ref,
                                     GPtrArray **out,
                                     GHashTable **semantic_out,
                                     dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  dt_iop_module_t *module = dt_remote_find_module(dev, ref, error);
  if(!module) return FALSE;

  // Keep scalar-only callers independent of the semantic registry. When a
  // semantic snapshot is requested, read it before allocating scalar output
  // so any registry/introspection failure leaves both outputs untouched.
  GHashTable *semantic_values = NULL;
  if(semantic_out && !_read_semantic_values(module, module->params, &semantic_values, error))
    return FALSE;

  GPtrArray *values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);

  dt_introspection_field_t *linear = module->so->get_introspection_linear();
  for(const dt_introspection_field_t *f = linear;
      linear && f->header.type != DT_INTROSPECTION_TYPE_NONE;
      f++)
  {
    dt_remote_value_t value;
    // Unsupported field types are already flagged writable:false in the
    // schema; here they are simply omitted from the value list, never
    // serialized from raw memory.
    if(!dt_remote_value_from_field(f, module->params, &value)) continue;

    dt_remote_patch_entry_t *entry = g_malloc0(sizeof(dt_remote_patch_entry_t));
    entry->name = g_strdup(f->header.name);
    entry->value = value;
    g_ptr_array_add(values, entry);
  }

  *out = values;
  if(semantic_out) *semantic_out = semantic_values;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* mutation API (plan step 7)                                              */
/* ---------------------------------------------------------------------- */

// After a remote mutation, reveal the edited module in the right panel --
// switch to its module group, expand it and give it focus -- so a watching
// user sees darktable being operated on, the same sequence as the GUI's
// show-module shortcut (_show_module_callback in develop/imageop.c). Gated
// on module->expander: it is NULL in headless contexts (unit tests) where
// dt_iop_request_focus() is not safe to call, and NULL is also how hidden
// GUIs opt out. dt_iop_gui_set_expanded()'s collapse_others branch
// *toggles* an already-expanded module when every other module is closed,
// so it is only called for a collapsed module.
static void _remote_reveal_module(dt_iop_module_t *module)
{
  if(!module || !module->expander) return;
  if(!dt_conf_get_bool("remote/follow_edited_module")) return;

  if(module->so->state == IOP_STATE_HIDDEN)
    dt_iop_gui_set_state(module, IOP_STATE_ACTIVE);

  if(!dt_iop_shown_in_group(module, dt_dev_modulegroups_get(module->dev)))
    dt_dev_modulegroups_switch(darktable.develop, module);

  if(!module->expanded)
    dt_iop_gui_set_expanded(module, TRUE, dt_conf_get_bool("darkroom/ui/single_module"));

  dt_iop_request_focus(module);
}

gboolean dt_remote_set_module_params(const dt_remote_module_ref_t *ref,
                                     const dt_remote_patch_t *patch,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out,
                                     dt_remote_error_t **error)
{
  // internals doc §1: every remote-edit call, mutations included, must run
  // inline on the GTK main thread -- cheap to assert, expensive to debug
  // if ever violated (a mutation racing the pixelpipe/GUI from another
  // thread). darktable.control is NULL only before dt_control_init() runs,
  // which is long before any darkroom image (and hence this call) can
  // exist, so the NULL check is defensive, not expected in practice.
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));

  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  // Step 1 (internals §3): revision CAS against the *current* image --
  // checked before locating the module, so a stale/foreign revision is
  // rejected without ever touching module lookup or params. First,
  // self-heal the tracker's image identity: if the tracker never learned
  // the current darkroom image (its DEVELOP_IMAGE_CHANGED can be raised
  // before dt_control_running() during startup and silently dropped),
  // stamp it now -- see dt_remote_revision_observe_image()'s header
  // comment. The stamp legitimately bumps (the tracker is observing a
  // state change it missed), so a client whose expected_revision predates
  // the healing gets a retryable revision_conflict, re-reads state (which
  // heals through the same call and hands out a revision that will
  // match), and succeeds -- instead of conflicting forever against an
  // imgid of NO_IMGID.
  dt_remote_revision_observe_image(dev->image_storage.id);
  if(expected_revision
     && !dt_remote_revision_matches(dt_remote_revision_current(), *expected_revision,
                                    dev->image_storage.id))
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_REVISION_CONFLICT,
                                   _("expected revision %" PRIu64 " does not match current state"),
                                   *expected_revision);
    return FALSE;
  }

  // Step 2: locate the existing module instance.
  dt_iop_module_t *module = dt_remote_find_module(dev, ref, error);
  if(!module) return FALSE;

  // Steps 3-6: allocate + copy the whole params block, then resolve,
  // convert, and validate every patch entry into that scratch copy --
  // dt_remote_patch_apply() is the pure core (unit-tested directly,
  // fixture-driven, no dt_develop_t); on any rejection, the scratch block
  // is simply freed and live params were never touched.
  void *temp_params = g_malloc(module->params_size);
  memcpy(temp_params, module->params, module->params_size);

  dt_introspection_field_t *linear = module->so->get_introspection_linear();
  if(!dt_remote_patch_apply(linear, dt_remote_denylist_for_op(module->op), patch, temp_params, error))
  {
    g_free(temp_params);
    return FALSE;
  }

  // Curve-design transaction steps 5-7: scalar values above establish the
  // projected state first; adapter preparation, semantic validation/native
  // writes, and completed-state validation then operate on the same scratch
  // block. Any failure discards both scalar and semantic changes before live
  // params, enable state, GUI, history, or revision are touched. Every row's
  // apply_patch() gets the whole patch (not a pre-partitioned slice) and is
  // called unconditionally, in table order (curve, then vector) -- each
  // engine partitions its own class's entries out internally and computes
  // its own prepare_needed gate from the whole patch (see the
  // dt_remote_class_ops_t.apply_patch field comment above); this preserves
  // today's error precedence and the "scalar-only patch can still reach an
  // engine via its prepare_fields" behavior exactly.
  for(gsize row = 0; row < G_N_ELEMENTS(s_class_ops); row++)
    if(!s_class_ops[row].apply_patch(module, module->params, temp_params, patch, error))
    {
      g_free(temp_params);
      return FALSE;
    }

  // Semantic read-back happens against the projected block, before the
  // commit: projected and post-commit params are byte-identical (the memcpy
  // below), and reading here means a read failure (registry drift racing
  // the transaction) still aborts with live state untouched. Restricted to
  // exactly the semantic IDs the patch wrote -- the same rule the scalar
  // read-back loop below applies to field names. One pass over the merged
  // (curve + vector) semantic_values map: entries are wrapped immediately
  // after each class's read so the filter below can key off `class_id`
  // directly, then trimmed to exactly what this patch wrote.
  GHashTable *semantic_readback = NULL;
  if(patch->semantic_values && patch->semantic_values->len > 0)
  {
    if(!_read_semantic_values(module, temp_params, &semantic_readback, error))
    {
      g_free(temp_params);
      return FALSE;
    }

    GHashTableIter it;
    gpointer key = NULL;
    gpointer val = NULL;
    g_hash_table_iter_init(&it, semantic_readback);
    while(g_hash_table_iter_next(&it, &key, &val))
    {
      const dt_remote_semantic_value_t *wrapped = val;
      gboolean written = FALSE;
      for(guint i = 0; !written && i < patch->semantic_values->len; i++)
      {
        const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
        if(!semantic || semantic->class_id != wrapped->class_id) continue;
        const char *patch_name = semantic->class_id == DT_REMOTE_PARAMETER_CURVE
                                    ? semantic->value.curve.name : semantic->value.vector.name;
        written = !g_strcmp0(patch_name, key);
      }
      if(!written) g_hash_table_iter_remove(&it);
    }

    // apply_patch just resolved every requested ID against the same
    // registry -- an entry missing from the read is drift between the two
    // calls, and the wire contract promises read-back for every written
    // entry, so fail the whole transaction rather than answer partially.
    if(g_hash_table_size(semantic_readback) != patch->semantic_values->len)
    {
      g_hash_table_unref(semantic_readback);
      g_free(temp_params);
      if(error)
        *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                     _("semantic read-back is missing a written entry"));
      return FALSE;
    }
  }

  // Step 8: copy the validated block to live params, and apply the
  // enable/disable tri-state in the same transaction -- parameters never
  // implicitly enable a disabled module (plan step 7's binding rule).
  memcpy(module->params, temp_params, module->params_size);
  g_free(temp_params);

  if(patch->has_enable) module->enabled = patch->enable ? TRUE : FALSE;

  // Step 9: sync the module GUI, then add exactly one history item for
  // the whole patch -- the preset-apply idiom (src/gui/presets.c:1074-
  // 1115): dt_iop_gui_update() manages its own ENTER/LEAVE guard and has
  // returned (guard released) before dt_dev_add_history_item() runs, so
  // the "never add a history item inside a GUI-update guard" rule
  // (src/develop/develop.c:1379) holds by construction, not by a separate
  // check here.
  //
  // Revision snapshot BEFORE the history item: DEVELOP_HISTORY_CHANGE is
  // delivered synchronously here -- g_main_context_invoke_full() invokes
  // the handler directly when the calling thread owns the default main
  // context (documented GLib behavior), and this function is asserted to
  // run on the GTK main thread -- so by the time
  // dt_dev_add_history_item() returns, the tracker's counter has already
  // advanced by exactly the one delivery dt_dev_undo_end_record()
  // raised. See _history_change_bump_and_stamp() in remote_revision.c
  // for why the earlier commit+suppress design around an assumed-async
  // delivery was wrong (live-verified double bump + a leaked slot that
  // swallowed a genuine undo).
  const uint64_t pre_revision = dt_remote_revision_get(dt_remote_revision_current());
  dt_iop_gui_update(module);
  dt_dev_add_history_item(dev, module, FALSE);
  if(module->widget) gtk_widget_queue_draw(module->widget);
  _remote_reveal_module(module);

  // Step 10: the tracker has already counted the synchronous delivery;
  // read the resulting revision. Defensive fallback: if the counter did
  // NOT advance (develop.c has gated paths that skip the raise, e.g.
  // history_postpone_invalidate, and future debounce changes could add
  // more), account for the change ourselves with a plain bump and NO
  // suppression bookkeeping. Design rule (fix round 2): a redundant bump
  // only costs a spurious *retryable* revision_conflict on the client's
  // next CAS; a swallowed bump would let a stale CAS silently clobber a
  // user edit -- always fail toward the former. Then read every value
  // back from the now-live module state (never echoed from `patch`),
  // restricted to exactly the field names the patch touched, matching
  // the wire contract's result shape.
  uint64_t new_revision = dt_remote_revision_get(dt_remote_revision_current());
  if(new_revision == pre_revision) new_revision = dt_remote_revision_force_bump();

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(module->op);
  result->instance = module->multi_priority;
  result->instance_name =
    dt_remote_instance_name_is_default(module) ? g_strdup("") : g_strdup(module->multi_name);
  result->enabled = module->enabled;
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *sent = g_ptr_array_index(patch->scalar_values, i);
    const dt_introspection_field_t *f = dt_remote_find_field(linear, sent->name);
    dt_remote_value_t value;
    if(!f || !dt_remote_value_from_field(f, module->params, &value)) continue;  // unreachable: just validated

    dt_remote_patch_entry_t *entry = g_malloc0(sizeof(dt_remote_patch_entry_t));
    entry->name = g_strdup(sent->name);
    entry->value = value;
    g_ptr_array_add(result->values, entry);
  }
  result->semantic_values = semantic_readback;  // already class-tagged (wrapped above)
  result->revision = new_revision;

  *out = result;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* mutation API (plan step 8)                                             */
/* ---------------------------------------------------------------------- */

// The shared front half of every step-8 mutation, identical to
// set_module_params' steps 1-2: assert main thread, require a darkroom
// image, self-heal the tracker's image identity, then the compare-and-swap
// against `expected_revision` (NULL = unconditional). On success returns
// TRUE with `*dev_out` set; on any failure allocates `*error` and returns
// FALSE having changed nothing. See dt_remote_set_module_params()'s inline
// comments for why each step is ordered this way (they are the normative
// source; this helper only de-duplicates them).
static gboolean dt_remote_mutation_precheck(const uint64_t *expected_revision,
                                            dt_develop_t **dev_out,
                                            dt_remote_error_t **error)
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));

  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  dt_remote_revision_observe_image(dev->image_storage.id);
  if(expected_revision
     && !dt_remote_revision_matches(dt_remote_revision_current(), *expected_revision,
                                    dev->image_storage.id))
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_REVISION_CONFLICT,
                                   _("expected revision %" PRIu64 " does not match current state"),
                                   *expected_revision);
    return FALSE;
  }

  if(dev_out) *dev_out = dev;
  return TRUE;
}

// Reads the tracker's counter after a darktable op that was expected to
// raise DEVELOP_HISTORY_CHANGE (delivered synchronously on this thread, so
// the counter has normally already advanced past `pre_revision`); if it did
// NOT advance -- develop.c has gated raise paths -- account for the change
// with a plain unpaired bump. Identical fallback rule to
// dt_remote_set_module_params() step 10; see dt_remote_revision_force_bump()
// for why redundant-bump (spurious retryable conflict) beats swallowed-bump
// (silent clobber).
static uint64_t dt_remote_read_new_revision(uint64_t pre_revision)
{
  uint64_t new_revision = dt_remote_revision_get(dt_remote_revision_current());
  if(new_revision == pre_revision) new_revision = dt_remote_revision_force_bump();
  return new_revision;
}

gboolean dt_remote_set_module_enabled(const dt_remote_module_ref_t *ref,
                                      gboolean enabled,
                                      const uint64_t *expected_revision,
                                      dt_remote_mutation_result_t **out,
                                      dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_mutation_precheck(expected_revision, &dev, error)) return FALSE;

  dt_iop_module_t *module = dt_remote_find_module(dev, ref, error);
  if(!module) return FALSE;

  // Toggle through the same two operations the on/off header callback
  // performs (_gui_off_callback: set module->enabled, then exactly one
  // dt_dev_add_history_item()); dt_iop_gui_update() resyncs the header's
  // enable button, matching the preset-apply idiom used by
  // set_module_params. Enabling is always its own single history item.
  const uint64_t pre_revision = dt_remote_revision_get(dt_remote_revision_current());
  module->enabled = enabled ? TRUE : FALSE;
  dt_iop_gui_update(module);
  dt_dev_add_history_item(dev, module, FALSE);
  if(module->widget) gtk_widget_queue_draw(module->widget);
  _remote_reveal_module(module);

  const uint64_t new_revision = dt_remote_read_new_revision(pre_revision);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(module->op);
  result->instance = module->multi_priority;
  // enable's wire result omits instance_name (with_instance_name=FALSE), so
  // leave result->instance_name NULL -- mutation_result_free tolerates it.
  result->enabled = module->enabled;
  result->values = NULL;  // enable carries no values on the wire
  result->revision = new_revision;

  *out = result;
  return TRUE;
}

gboolean dt_remote_reset_module(const dt_remote_module_ref_t *ref,
                                const uint64_t *expected_revision,
                                dt_remote_mutation_result_t **out,
                                dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_mutation_precheck(expected_revision, &dev, error)) return FALSE;

  dt_iop_module_t *module = dt_remote_find_module(dev, ref, error);
  if(!module) return FALSE;

  // The reset-button lifecycle (_gui_reset_callback's reset branch, minus
  // the Ctrl auto-preset shortcut): drop any drawn mask first (before
  // reload_defaults resets the blend params that name it), reload the
  // image-specific default params + blend params
  // (dt_iop_reload_defaults -> dt_iop_load_default_params), let the module
  // reset its own gui, resync the widgets, then exactly one history item.
  const uint64_t pre_revision = dt_remote_revision_get(dt_remote_revision_current());

  if(dt_is_valid_maskid(module->blend_params->mask_id))
  {
    dt_masks_form_t *grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(grp) dt_masks_form_remove(module, NULL, grp);
  }
  dt_iop_reload_defaults(module);
  dt_iop_gui_reset(module);
  dt_iop_gui_update(module);
  dt_dev_add_history_item(dev, module, TRUE);
  if(module->widget) gtk_widget_queue_draw(module->widget);
  _remote_reveal_module(module);

  const uint64_t new_revision = dt_remote_read_new_revision(pre_revision);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(module->op);
  result->instance = module->multi_priority;
  // reset's wire result omits instance_name (with_instance_name=FALSE), so
  // leave result->instance_name NULL -- mutation_result_free tolerates it.
  result->enabled = module->enabled;

  // Post-reset scalar values of every supported field, read back from live
  // params (never echoed) -- same read-back walk as get_module_params.
  result->values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  dt_introspection_field_t *linear = module->so->get_introspection_linear();
  for(const dt_introspection_field_t *f = linear;
      linear && f->header.type != DT_INTROSPECTION_TYPE_NONE;
      f++)
  {
    dt_remote_value_t value;
    if(!dt_remote_value_from_field(f, module->params, &value)) continue;
    dt_remote_patch_entry_t *entry = g_malloc0(sizeof(dt_remote_patch_entry_t));
    entry->name = g_strdup(f->header.name);
    entry->value = value;
    g_ptr_array_add(result->values, entry);
  }
  result->revision = new_revision;

  *out = result;
  return TRUE;
}

gboolean dt_remote_create_module_instance(const dt_remote_module_ref_t *ref,
                                          gboolean copy_params,
                                          const uint64_t *expected_revision,
                                          dt_remote_mutation_result_t **out,
                                          dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_mutation_precheck(expected_revision, &dev, error)) return FALSE;

  // `ref` addresses the source (base) instance to duplicate.
  dt_iop_module_t *base = dt_remote_find_module(dev, ref, error);
  if(!base) return FALSE;

  if(base->flags() & IOP_FLAGS_ONE_INSTANCE)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED,
                                   _("module '%s' does not support multiple instances"), base->op);
    return FALSE;
  }

  // dt_iop_gui_duplicate() adds two history items (the base's current state
  // and the new instance's creation) -- both raise DEVELOP_HISTORY_CHANGE
  // synchronously, so the tracker advances twice; the fallback only fires
  // if the counter did not move at all.
  const uint64_t pre_revision = dt_remote_revision_get(dt_remote_revision_current());
  dt_iop_module_t *module = dt_iop_gui_duplicate(base, copy_params);
  if(!module)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL,
                                   _("could not create a new instance of module '%s'"), base->op);
    return FALSE;
  }
  // dt_iop_gui_duplicate() already expands and focuses the new instance
  // but never switches module groups; the reveal adds the group switch and
  // is a no-op for the rest.
  _remote_reveal_module(module);

  const uint64_t new_revision = dt_remote_read_new_revision(pre_revision);

  dt_remote_mutation_result_t *result = g_malloc0(sizeof(dt_remote_mutation_result_t));
  result->op = g_strdup(module->op);
  result->instance = module->multi_priority;
  result->instance_name =
    dt_remote_instance_name_is_default(module) ? g_strdup("") : g_strdup(module->multi_name);
  result->enabled = module->enabled;
  result->values = NULL;  // create carries no values on the wire
  result->revision = new_revision;

  *out = result;
  return TRUE;
}

gboolean dt_remote_get_history(int limit,
                               GPtrArray **items_out,
                               uint64_t *revision_out,
                               dt_remote_error_t **error)
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));

  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  // Model-oriented metadata only: no param blobs. Items are ordered
  // oldest->newest (pipe/history-list order); with more entries than
  // `limit`, only the newest `limit` are returned, each keeping its true
  // stack position in `seq`. `limit` is already clamped to [1,100] by the
  // caller; guard defensively anyway.
  if(limit < 1) limit = 1;
  if(limit > 100) limit = 100;

  // Only the first `history_end` entries are the currently-active edit chain:
  // a history-panel "jump to an earlier point" sets dev->history_end without
  // truncating dev->history, so "future" (redoable) entries linger past the
  // active boundary. Clamping keeps get_history's items/seq coherent with the
  // revision that describes the live state. Defensively cap to the real list
  // length so a transiently-larger history_end can't over-run the walk.
  const int list_len = (int)g_list_length(dev->history);
  int total = list_len;
  if(dev->history_end >= 0 && dev->history_end < total) total = dev->history_end;
  const int skip = (total > limit) ? (total - limit) : 0;

  GPtrArray *items = g_ptr_array_new_with_free_func(dt_remote_history_item_free);
  int seq = 0;
  for(GList *h = dev->history; h && seq < total; h = g_list_next(h), seq++)
  {
    if(seq < skip) continue;
    const dt_dev_history_item_t *hist = h->data;

    dt_remote_history_item_t *item = g_malloc0(sizeof(dt_remote_history_item_t));
    item->seq = seq;
    item->op = g_strdup(hist->op_name);
    item->instance = hist->multi_priority;
    item->display_name = g_strdup(hist->module ? hist->module->name() : hist->op_name);
    // multi_name[0] empty (or legacy "0") == the default, unnamed instance,
    // same convention dt_remote_instance_name_is_default() applies to a
    // live module.
    item->instance_name =
      (hist->multi_name[0] == '\0' || !strcmp(hist->multi_name, "0")) ? g_strdup("")
                                                                      : g_strdup(hist->multi_name);
    item->enabled = hist->enabled;
    g_ptr_array_add(items, item);
  }

  if(revision_out) *revision_out = dt_remote_revision_observe_image(dev->image_storage.id);
  *items_out = items;
  return TRUE;
}

gboolean dt_remote_undo(uint64_t expected_revision,
                        uint64_t *revision_out,
                        dt_remote_error_t **error)
{
  // expected_revision is required for undo (compare-and-undo); pass it as a
  // non-NULL pointer to the shared precheck so a mismatch is the standard
  // revision_conflict, and nothing is undone.
  dt_develop_t *dev = NULL;
  if(!dt_remote_mutation_precheck(&expected_revision, &dev, error)) return FALSE;

  // The same undo entry point as Ctrl+Z in the darkroom
  // (_darkroom_undo_callback): one transition, scoped to develop. It raises
  // DEVELOP_HISTORY_CHANGE synchronously, so the tracker advances; the
  // fallback covers the gated-raise case.
  const uint64_t pre_revision = dt_remote_revision_get(dt_remote_revision_current());
  dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP);
  const uint64_t new_revision = dt_remote_read_new_revision(pre_revision);

  if(revision_out) *revision_out = new_revision;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* preview rendering (plan step 9, internals §8)                           */
/* ---------------------------------------------------------------------- */

void dt_remote_preview_free(dt_remote_preview_t *preview)
{
  if(!preview) return;
  g_free(preview->jpeg);
  g_free(preview);
}

uint64_t dt_remote_current_revision(void)
{
  // The live process-local revision counter, read on the main thread at
  // preview-completion time to detect whether darkroom state drifted while a
  // background render was in flight (internals §8's coherence check). Reads
  // the same self-healing singleton every other main-thread revision path
  // uses; 0 when no server is connected (dt_remote_revision_get is NULL-safe),
  // which the completion treats as "changed" and fails toward a retryable
  // error -- the branch's fail-toward-retryable rule.
  return dt_remote_revision_get(dt_remote_revision_current());
}

gboolean dt_remote_render_preview_prepare(dt_remote_preview_request_t *out,
                                          dt_remote_error_t **error)
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));

  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  // Binding caveat (internals §8): the export path re-loads history from
  // the database (dt_imageio_export_with_flags builds its own
  // dt_develop_t), so any not-yet-written live history must be flushed
  // first -- and the revision is captured at this same instant, because
  // that is the state the background render will actually see.
  dt_dev_write_history(dev);

  out->imgid = dev->image_storage.id;
  out->revision = dt_remote_revision_observe_image(dev->image_storage.id);
  return TRUE;
}

// Synthetic in-memory format sink for dt_imageio_export_with_flags(),
// mirroring the two in-tree precedents (_preview_write_image in
// imageio.c, the HDR-merge job's ad-hoc format in control_jobs.c): the
// export drives the pixelpipe and hands write_image() the final 8-bit
// buffer, which is copied out instead of written to a file. `filename`
// is an ignored constant throughout.
typedef struct _remote_preview_sink_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;         // g_malloc'd RGBA copy, 4 * width * height
  int width, height;    // final processed dimensions
} _remote_preview_sink_t;

static int _remote_preview_bpp(dt_imageio_module_data_t *data)
{
  (void)data;
  return 8;
}

static int _remote_preview_levels(dt_imageio_module_data_t *data)
{
  (void)data;
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *_remote_preview_mime(dt_imageio_module_data_t *data)
{
  (void)data;
  return "memory";
}

static int _remote_preview_write_image(dt_imageio_module_data_t *data,
                                       const char *filename,
                                       const void *in,
                                       const dt_colorspaces_color_profile_type_t over_type,
                                       const char *over_filename,
                                       void *exif,
                                       const int exif_len,
                                       const dt_imgid_t imgid,
                                       const int num,
                                       const int total,
                                       dt_dev_pixelpipe_t *pipe,
                                       const gboolean export_masks)
{
  (void)filename;
  (void)over_type;
  (void)over_filename;
  (void)exif;
  (void)exif_len;
  (void)imgid;
  (void)num;
  (void)total;
  (void)pipe;
  (void)export_masks;

  _remote_preview_sink_t *d = (_remote_preview_sink_t *)data;
  if(!in || data->width <= 0 || data->height <= 0) return 1;

  const size_t size = sizeof(uint32_t) * (size_t)data->width * data->height;
  d->buf = g_malloc(size);
  memcpy(d->buf, in, size);
  d->width = data->width;
  d->height = data->height;
  return 0;
}

gboolean dt_remote_render_preview_execute(const dt_remote_preview_request_t *req,
                                          int max_px, int quality,
                                          GCancellable *cancellable,
                                          dt_remote_preview_t **out,
                                          dt_remote_error_t **error)
{
  // Deliberately NO main-thread assert: this is the one remote_edit entry
  // point designed to run on a background job thread (internals §1/§8).
  // It never touches darktable.develop -- the export builds its own
  // dt_develop_t from the history prepare() flushed to the database.
  if(!req || !out)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL, _("internal error: null argument"));
    return FALSE;
  }

  if(g_cancellable_is_cancelled(cancellable))
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_PREVIEW_FAILED, _("preview render was cancelled"));
    return FALSE;
  }

  dt_imageio_module_format_t format = {
    .mime = _remote_preview_mime,
    .levels = _remote_preview_levels,
    .bpp = _remote_preview_bpp,
    .write_image = _remote_preview_write_image,
  };

  _remote_preview_sink_t sink = { 0 };   // zero head: empty style, style_append FALSE
  sink.head.max_width = max_px;          // bounds the longest edge (upscale FALSE below)
  sink.head.max_height = max_px;

  // Flag choices per internals §8 (binding): ignore_exif TRUE (no blob in
  // a preview), display_byteorder FALSE (the 8-bit path then emits RGBA --
  // exactly the layout dt_imageio_jpeg_compress() reads), high_quality
  // FALSE, upscale FALSE (a small image stays its native size; max_px is a
  // cap, not a target), thumbnail_export FALSE, sRGB output profile
  // (portable interchange -- the display profile would be wrong
  // off-machine), history_end -1 (the full current history).
  const gboolean export_failed = dt_imageio_export_with_flags(
    (dt_imgid_t)req->imgid, "remote-preview", &format, &sink.head,
    TRUE /*ignore_exif*/, FALSE /*display_byteorder*/, FALSE /*high_quality*/,
    FALSE /*upscale*/, FALSE /*is_scaling*/, 1.0 /*scale_factor*/,
    FALSE /*thumbnail_export*/, NULL /*filter*/, FALSE /*copy_metadata*/,
    FALSE /*export_masks*/, DT_COLORSPACE_SRGB, NULL /*icc_filename*/,
    DT_INTENT_LAST, NULL /*storage*/, NULL /*storage_params*/, 1, 1,
    NULL /*metadata*/, -1 /*history_end*/);

  if(export_failed || !sink.buf)
  {
    g_free(sink.buf);
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_PREVIEW_FAILED, _("pixelpipe export failed"));
    return FALSE;
  }

  if(g_cancellable_is_cancelled(cancellable))
  {
    g_free(sink.buf);
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_PREVIEW_FAILED, _("preview render was cancelled"));
    return FALSE;
  }

  // dt_imageio_jpeg_compress()'s contract: caller allocates out >= 4*w*h;
  // it returns the encoded byte length, or 1 from its setjmp error path.
  const size_t cap = sizeof(uint32_t) * (size_t)sink.width * sink.height;
  uint8_t *jpeg = g_malloc(cap);
  const int jpeg_len = dt_imageio_jpeg_compress(sink.buf, jpeg, sink.width, sink.height, quality);
  g_free(sink.buf);

  if(jpeg_len <= 1 || (size_t)jpeg_len > cap)
  {
    g_free(jpeg);
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_PREVIEW_FAILED, _("JPEG encoding failed"));
    return FALSE;
  }

  dt_remote_preview_t *preview = g_malloc0(sizeof(dt_remote_preview_t));
  preview->jpeg = g_realloc(jpeg, (size_t)jpeg_len);  // shed the 4*w*h allocation slack
  preview->jpeg_len = (size_t)jpeg_len;
  preview->width = sink.width;
  preview->height = sink.height;
  preview->revision = req->revision;

  *out = preview;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
