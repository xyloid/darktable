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
#include "control/control.h"
#include "control/remote_revision.h"
#include "develop/develop.h"
#include "develop/imageop.h"
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

static gboolean dt_remote_denylisted(const dt_remote_denylist_t *denylist, const char *name)
{
  if(!denylist || !denylist->names || !name) return FALSE;
  for(const char *const *n = denylist->names; *n; n++)
    if(!g_strcmp0(*n, name)) return TRUE;
  return FALSE;
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
  if(!patch || !patch->scalar_values || patch->scalar_values->len == 0)
  {
    if(error) *error = dt_remote_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("values must be non-empty"));
    return FALSE;
  }

  for(guint i = 0; i < patch->scalar_values->len; i++)
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

  // Step 6 ("validate the completed parameter block"): a no-op for v1's
  // scalar-only patches -- every field is already individually validated
  // above and the v1 schema has no cross-field constraints. See the
  // header comment: this is the extension point future semantic-value
  // classes (patch->semantic_values, reserved/unused here) hook into.
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
// dt_remote_require_darkroom_image(); the protocol reference's error
// matrix allows only unknown_module/invalid_value here.
gboolean dt_remote_get_module_schema(const char *op,
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
  schema->fields = dt_remote_schema_from_introspection(linear, NULL);

  *out = schema;
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
                                     dt_remote_error_t **error)
{
  dt_develop_t *dev = NULL;
  if(!dt_remote_require_darkroom_image(&dev, error)) return FALSE;

  dt_iop_module_t *module = dt_remote_find_module(dev, ref, error);
  if(!module) return FALSE;

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
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* mutation API (plan step 7)                                              */
/* ---------------------------------------------------------------------- */

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
  if(!dt_remote_patch_apply(linear, NULL, patch, temp_params, error))
  {
    g_free(temp_params);
    return FALSE;
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
  result->revision = new_revision;

  *out = result;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
