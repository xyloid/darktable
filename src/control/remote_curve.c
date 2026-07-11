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

#include "control/remote_curve.h"

#include "common/darktable.h" // _()

#include <stdarg.h>

/* ---------------------------------------------------------------------- */
/* error helper                                                           */
/* ---------------------------------------------------------------------- */

// Local to this file: remote_edit.c's dt_remote_error_new() is static to
// that translation unit, so remote_curve.c mirrors the same idiom rather
// than exporting a shared constructor for one internal error code.
static dt_remote_error_t *dt_remote_curve_error_new(dt_remote_error_code_t code,
                                                    const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_curve_error_new(dt_remote_error_code_t code,
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

/* ---------------------------------------------------------------------- */
/* introspection path cursor                                              */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_path_resolve(const dt_remote_introspection_path_t *path,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                const dt_introspection_field_t **out_field,
                                void **out_ptr,
                                dt_remote_error_t **error)
{
  if(!path || !root || !params_blob)
  {
    if(error)
      *error = dt_remote_curve_error_new(DT_REMOTE_ERR_INTERNAL,
                                         _("internal error: null argument to introspection path resolver"));
    return FALSE;
  }

  // dt_introspection_get_child()/dt_introspection_access_array() are not
  // declared const-correct (they mutate nothing; the introspection tree
  // is process-lifetime static data), so cast away const at the call
  // site rather than changing this function's public signature -- same
  // idiom as dt_introspection_get_enum_name((dt_introspection_field_t *)f, ...)
  // in remote_edit.c.
  dt_introspection_field_t *field = (dt_introspection_field_t *)root;
  void *ptr = params_blob;

  for(guint i = 0; i < path->length; i++)
  {
    const dt_remote_path_segment_t *segment = &path->segments[i];
    dt_introspection_field_t *child = NULL;
    void *next = NULL;

    switch(segment->type)
    {
      case DT_REMOTE_PATH_FIELD:
        next = dt_introspection_get_child(field, ptr, segment->value.field, &child);
        break;

      case DT_REMOTE_PATH_INDEX:
        next = dt_introspection_access_array(field, ptr, segment->value.index, &child);
        break;

      default:
        next = NULL;
        break;
    }

    if(!next)
    {
      if(error)
        *error = dt_remote_curve_error_new(
          DT_REMOTE_ERR_INTERNAL,
          _("internal error: introspection path resolution failed at segment %u of %u"),
          i, path->length);
      return FALSE;
    }

    field = child;
    ptr = next;
  }

  if(out_field) *out_field = field;
  if(out_ptr) *out_ptr = ptr;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
