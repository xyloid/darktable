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

// Introspection-touching engine pieces for the darktable MCP remote-edit
// semantic-curve machinery: the bounds-checked path cursor that walks a
// static, compiled-in path description against a module's introspection
// tree and a live params blob. The registry/validator/adapter layers that
// build on top of this cursor land in later tasks; see the curve-classes
// design doc §Registry descriptors / §Introspection path for the
// normative shapes this header copies verbatim.
//
// Neutral (non-introspection) curve/patch types live in
// control/remote_parameters.h, not here -- see that header's comment for
// the file split rationale.
//
// Threading: like remote_edit.h, any code that resolves a path against a
// live darkroom module's params block must do so from the GTK main
// thread; the path/segment types themselves are inert compiled-in data
// with no such constraint.

#pragma once

#include "common/introspection.h"
#include "control/remote_edit.h" // dt_remote_error_t

#include <glib.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* introspection path                                                      */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_path_segment_type_t
{
  DT_REMOTE_PATH_FIELD,
  DT_REMOTE_PATH_INDEX
} dt_remote_path_segment_type_t;

typedef struct dt_remote_path_segment_t
{
  dt_remote_path_segment_type_t type;
  union
  {
    const char *field;
    guint index;
  } value;
} dt_remote_path_segment_t;

typedef struct dt_remote_introspection_path_t
{
  const dt_remote_path_segment_t *segments;
  guint length;
} dt_remote_introspection_path_t;

// Paths are compiled static data and cannot originate from a remote
// request. They resolve through dt_introspection_get_child() and
// dt_introspection_access_array(), checking type and bounds at every
// segment.

/** walks `path` against `root`'s introspection tree and the matching
 * offsets inside `params_blob`, checking a field/struct-or-union type
 * before every DT_REMOTE_PATH_FIELD descent and array bounds before
 * every DT_REMOTE_PATH_INDEX descent.
 *
 * On success returns TRUE and, if non-%NULL, sets `*out_field` to the
 * leaf introspection field descriptor and `*out_ptr` to the matching
 * pointer inside `params_blob`. Neither pointer is retained by this
 * function past the call: `*out_field` points into the module .so's
 * static introspection data (process lifetime) and `*out_ptr` points
 * into the caller-owned `params_blob`.
 *
 * On failure returns FALSE and, if `error` is non-%NULL, sets `*error`
 * to a newly allocated DT_REMOTE_ERR_INTERNAL error (caller frees with
 * dt_remote_error_free()). Every failure here is internal: `path` is
 * compiled-in registry data, so a segment that fails to resolve (wrong
 * field type, missing child, out-of-bounds index, type drift) is
 * registry/introspection drift, never caller error. */
gboolean dt_remote_path_resolve(const dt_remote_introspection_path_t *path,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                const dt_introspection_field_t **out_field,
                                void **out_ptr,
                                dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
