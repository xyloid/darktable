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
#include "develop/imageop.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

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

// Builds a DT_REMOTE_ERR_INVALID_VALUE with a details_json object carrying
// "parameter" (always), "constraint" (always, one of the short stable
// strings documented on dt_remote_curve_validate()), and "point_index"
// (only when point_index >= 0 -- the documented convention: omitted for
// whole-array failures and for interpolation_not_allowed, present and
// 0-based otherwise).
//
// The JSON is hand-built with g_strdup_printf() rather than a JSON library:
// remote_curve* files must never include JSON, socket, or MCP headers, and
// every value placed into this string is either a compiled-in descriptor
// name (dt_remote_curve_descriptor_t::name, never request-controlled), a
// small non-negative int (point_index), or one of the fixed C string
// literals passed as `constraint` by callers in this file (e.g.
// "min_points", "non_finite", "adjacent_spacing"). None of these can
// contain characters that require JSON escaping, so no escaping helper is
// used or introduced.
static dt_remote_error_t *dt_remote_curve_validate_error_new(const char *parameter, int point_index,
                                                             const char *constraint,
                                                             const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *dt_remote_curve_validate_error_new(const char *parameter, int point_index,
                                                             const char *constraint,
                                                             const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = DT_REMOTE_ERR_INVALID_VALUE;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  if(point_index >= 0)
    error->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"point_index\":%d,\"constraint\":\"%s\"}",
                                          parameter ? parameter : "", point_index, constraint);
  else
    error->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"constraint\":\"%s\"}",
                                          parameter ? parameter : "", constraint);

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

/* ---------------------------------------------------------------------- */
/* common curve validator (Validation algorithm items 3-10 only)          */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_curve_validate(const dt_remote_curve_descriptor_t *desc,
                                  const GArray *points,
                                  gboolean has_interpolation,
                                  dt_remote_curve_interpolation_t interpolation,
                                  dt_remote_error_t **error)
{
  if(!desc)
  {
    if(error)
      *error = dt_remote_curve_error_new(DT_REMOTE_ERR_INTERNAL,
                                         _("internal error: null curve descriptor"));
    return FALSE;
  }

  const char *parameter = desc->name ? desc->name : "";
  const guint n = points ? points->len : 0;

  /* item 3: point count against descriptor bounds (global request-size
   * limits are a protocol-layer concern, not this function's). */
  if(n < desc->minimum_points)
  {
    if(error)
      *error = dt_remote_curve_validate_error_new(
        parameter, -1, "min_points",
        _("curve '%s' has %u point(s); at least %u required"), parameter, n, desc->minimum_points);
    return FALSE;
  }
  if(n > desc->maximum_points)
  {
    if(error)
      *error = dt_remote_curve_validate_error_new(
        parameter, -1, "max_points",
        _("curve '%s' has %u point(s); at most %u allowed"), parameter, n, desc->maximum_points);
    return FALSE;
  }

  /* item 4: reject NaN/infinity, then values outside the x/y domain. One
   * pass over every point before moving on to order/spacing/boundary
   * checks below -- so a domain violation on an early point is always
   * reported before an order violation on a later one, matching the
   * algorithm's item ordering. */
  for(guint i = 0; i < n; i++)
  {
    const dt_remote_curve_point_t *p = &g_array_index(points, dt_remote_curve_point_t, i);

    if(!isfinite(p->x) || !isfinite(p->y))
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, (int)i, "non_finite",
          _("curve '%s' point %u has a non-finite coordinate"), parameter, i);
      return FALSE;
    }
    if(p->x < desc->x.minimum || p->x > desc->x.maximum)
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, (int)i, "domain_x",
          _("curve '%s' point %u has x=%.17g outside domain [%.17g, %.17g]"),
          parameter, i, p->x, desc->x.minimum, desc->x.maximum);
      return FALSE;
    }
    if(p->y < desc->y.minimum || p->y > desc->y.maximum)
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, (int)i, "domain_y",
          _("curve '%s' point %u has y=%.17g outside domain [%.17g, %.17g]"),
          parameter, i, p->y, desc->y.minimum, desc->y.maximum);
      return FALSE;
    }
  }

  /* item 5: strict ascending x when configured. A repeated x is reported
   * distinctly ("duplicate_x") from an actual decrease ("strict_order"). */
  if(desc->strict_x_order)
  {
    for(guint i = 1; i < n; i++)
    {
      const double x_prev = g_array_index(points, dt_remote_curve_point_t, i - 1).x;
      const double x_cur = g_array_index(points, dt_remote_curve_point_t, i).x;

      if(x_cur == x_prev)
      {
        if(error)
          *error = dt_remote_curve_validate_error_new(
            parameter, (int)i, "duplicate_x",
            _("curve '%s' points %u and %u share x=%.17g"), parameter, i - 1, i, x_cur);
        return FALSE;
      }
      if(x_cur < x_prev)
      {
        if(error)
          *error = dt_remote_curve_validate_error_new(
            parameter, (int)i, "strict_order",
            _("curve '%s' point %u (x=%.17g) is not strictly greater than point %u (x=%.17g)"),
            parameter, i, x_cur, i - 1, x_prev);
        return FALSE;
      }
    }
  }

  /* item 6: adjacent minimum spacing, skipped entirely when the rule is
   * DT_REMOTE_SPACING_NONE. Assumes ascending order (guaranteed above when
   * strict_x_order is set -- the only configuration exercised here). */
  if(desc->adjacent_spacing_rule != DT_REMOTE_SPACING_NONE)
  {
    for(guint i = 1; i < n; i++)
    {
      const double x_prev = g_array_index(points, dt_remote_curve_point_t, i - 1).x;
      const double x_cur = g_array_index(points, dt_remote_curve_point_t, i).x;
      const double delta = x_cur - x_prev;
      const gboolean ok = (desc->adjacent_spacing_rule == DT_REMOTE_SPACING_AT_LEAST)
        ? (delta >= desc->minimum_x_spacing)
        : (delta > desc->minimum_x_spacing);

      if(!ok)
      {
        if(error)
          *error = dt_remote_curve_validate_error_new(
            parameter, (int)i, "adjacent_spacing",
            _("curve '%s' points %u and %u are %.17g apart; minimum is %.17g"),
            parameter, i - 1, i, delta, desc->minimum_x_spacing);
        return FALSE;
      }
    }
  }

  /* item 7: periodic wrap spacing between the last and first point across
   * the domain wrap, skipped entirely when the rule is
   * DT_REMOTE_SPACING_NONE (rgbcurve's case). The wrap gap is the distance
   * from the last point up to the domain maximum, plus the distance from
   * the domain minimum to the first point -- zero iff the first and last
   * points sit exactly on the domain edges. */
  if(n > 0 && desc->wrap_spacing_rule != DT_REMOTE_SPACING_NONE)
  {
    const double x_first = g_array_index(points, dt_remote_curve_point_t, 0).x;
    const double x_last = g_array_index(points, dt_remote_curve_point_t, n - 1).x;
    const double wrap_delta = (x_first - desc->x.minimum) + (desc->x.maximum - x_last);
    const gboolean ok = (desc->wrap_spacing_rule == DT_REMOTE_SPACING_AT_LEAST)
      ? (wrap_delta >= desc->minimum_wrap_spacing)
      : (wrap_delta > desc->minimum_wrap_spacing);

    if(!ok)
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, 0, "wrap_spacing",
          _("curve '%s' periodic wrap gap is %.17g; minimum is %.17g"),
          parameter, wrap_delta, desc->minimum_wrap_spacing);
      return FALSE;
    }
  }

  /* item 8: domain-boundary point policy. OPTIONAL checks nothing;
   * REQUIRED pins first/last x to the domain edges (y unconstrained);
   * FIXED_IDENTITY additionally pins first/last y. */
  if(n > 0 && desc->boundary_point_policy != DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL)
  {
    const dt_remote_curve_point_t *first = &g_array_index(points, dt_remote_curve_point_t, 0);
    const dt_remote_curve_point_t *last = &g_array_index(points, dt_remote_curve_point_t, n - 1);

    if(first->x != desc->x.minimum)
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, 0, "boundary_policy",
          _("curve '%s' first point x=%.17g must equal domain minimum %.17g"),
          parameter, first->x, desc->x.minimum);
      return FALSE;
    }
    if(last->x != desc->x.maximum)
    {
      if(error)
        *error = dt_remote_curve_validate_error_new(
          parameter, (int)(n - 1), "boundary_policy",
          _("curve '%s' last point x=%.17g must equal domain maximum %.17g"),
          parameter, last->x, desc->x.maximum);
      return FALSE;
    }

    if(desc->boundary_point_policy == DT_REMOTE_CURVE_BOUNDARY_POINTS_FIXED_IDENTITY)
    {
      if(first->y != desc->y.minimum)
      {
        if(error)
          *error = dt_remote_curve_validate_error_new(
            parameter, 0, "boundary_policy",
            _("curve '%s' first point y=%.17g must equal domain minimum %.17g"),
            parameter, first->y, desc->y.minimum);
        return FALSE;
      }
      if(last->y != desc->y.maximum)
      {
        if(error)
          *error = dt_remote_curve_validate_error_new(
            parameter, (int)(n - 1), "boundary_policy",
            _("curve '%s' last point y=%.17g must equal domain maximum %.17g"),
            parameter, last->y, desc->y.maximum);
        return FALSE;
      }
    }
  }

  /* items 9-10: interpolation. Resolving/preserving the *current*
   * interpolation when none is supplied needs native introspection and is
   * handled by the apply engine below; this pure helper only checks the
   * allowlist when the caller supplied a value. */
  const gint64 interpolation_value = (gint64)interpolation;
  const guint interpolation_mask_width = sizeof(desc->interpolation_mask) * CHAR_BIT;
  if(has_interpolation
     && (interpolation_value < 0
         || (guint64)interpolation_value >= interpolation_mask_width))
  {
    if(error)
      *error = dt_remote_curve_validate_error_new(
        parameter, -1, "interpolation_not_allowed",
        _("curve '%s' does not allow interpolation %d"), parameter, (int)interpolation);
    return FALSE;
  }
  if(has_interpolation && !(desc->interpolation_mask & (1u << interpolation)))
  {
    if(error)
      *error = dt_remote_curve_validate_error_new(
        parameter, -1, "interpolation_not_allowed",
        _("curve '%s' does not allow interpolation %d"), parameter, (int)interpolation);
    return FALSE;
  }

  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* semantic curve mutation engine                                         */
/* ---------------------------------------------------------------------- */

static void deliver_curve_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

static dt_remote_error_t *curve_parameter_error_new(dt_remote_error_code_t code,
                                                    const char *parameter,
                                                    const char *constraint,
                                                    const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *curve_parameter_error_new(dt_remote_error_code_t code,
                                                    const char *parameter,
                                                    const char *constraint,
                                                    const char *format, ...)
{
  dt_remote_error_t *error = g_new0(dt_remote_error_t, 1);
  error->code = code;
  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);
  if(parameter && constraint)
    error->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"constraint\":\"%s\"}",
                                          parameter, constraint);
  return error;
}

static gboolean is_curve_integer_leaf(dt_introspection_type_t type)
{
  switch(type)
  {
    case DT_INTROSPECTION_TYPE_CHAR:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_LONG:
    case DT_INTROSPECTION_TYPE_ULONG:
    case DT_INTROSPECTION_TYPE_ENUM:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean is_curve_floating_leaf(dt_introspection_type_t type)
{
  return type == DT_INTROSPECTION_TYPE_FLOAT || type == DT_INTROSPECTION_TYPE_DOUBLE;
}

static gboolean read_curve_integer_leaf(const dt_introspection_field_t *field,
                                        const void *ptr,
                                        gint64 *out)
{
  if(!field || !ptr || !out) return FALSE;
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_CHAR: *out = *(const char *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_INT8: *out = *(const int8_t *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_UINT8: *out = *(const uint8_t *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_SHORT: *out = *(const short *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_USHORT: *out = *(const unsigned short *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_INT: *out = *(const int *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_UINT: *out = *(const unsigned int *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_LONG: *out = *(const long *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_ULONG: *out = (gint64)*(const unsigned long *)ptr; return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM: *out = *(const int *)ptr; return TRUE;
    default: return FALSE;
  }
}

static gboolean write_curve_integer_leaf(const dt_introspection_field_t *field,
                                         void *ptr,
                                         gint64 value)
{
  if(!field || !ptr) return FALSE;
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_CHAR: *(char *)ptr = (char)value; return TRUE;
    case DT_INTROSPECTION_TYPE_INT8: *(int8_t *)ptr = (int8_t)value; return TRUE;
    case DT_INTROSPECTION_TYPE_UINT8: *(uint8_t *)ptr = (uint8_t)value; return TRUE;
    case DT_INTROSPECTION_TYPE_SHORT: *(short *)ptr = (short)value; return TRUE;
    case DT_INTROSPECTION_TYPE_USHORT: *(unsigned short *)ptr = (unsigned short)value; return TRUE;
    case DT_INTROSPECTION_TYPE_INT: *(int *)ptr = (int)value; return TRUE;
    case DT_INTROSPECTION_TYPE_UINT: *(unsigned int *)ptr = (unsigned int)value; return TRUE;
    case DT_INTROSPECTION_TYPE_LONG: *(long *)ptr = (long)value; return TRUE;
    case DT_INTROSPECTION_TYPE_ULONG: *(unsigned long *)ptr = (unsigned long)value; return TRUE;
    case DT_INTROSPECTION_TYPE_ENUM: *(int *)ptr = (int)value; return TRUE;
    default: return FALSE;
  }
}

static gboolean write_curve_float_leaf(const dt_introspection_field_t *field,
                                       void *ptr,
                                       double value)
{
  if(!field || !ptr) return FALSE;
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: *(float *)ptr = (float)value; return TRUE;
    case DT_INTROSPECTION_TYPE_DOUBLE: *(double *)ptr = value; return TRUE;
    default: return FALSE;
  }
}

static gboolean map_native_curve_interpolation(gint64 native,
                                               dt_remote_curve_interpolation_t *out)
{
  switch(native)
  {
    case 0: *out = DT_REMOTE_CURVE_CUBIC_SPLINE; return TRUE;
    case 1: *out = DT_REMOTE_CURVE_CATMULL_ROM; return TRUE;
    case 2: *out = DT_REMOTE_CURVE_MONOTONE_HERMITE; return TRUE;
    default: return FALSE;
  }
}

static gboolean map_remote_curve_interpolation(dt_remote_curve_interpolation_t interpolation,
                                               gint64 *out)
{
  switch(interpolation)
  {
    case DT_REMOTE_CURVE_CUBIC_SPLINE: *out = 0; return TRUE;
    case DT_REMOTE_CURVE_CATMULL_ROM: *out = 1; return TRUE;
    case DT_REMOTE_CURVE_MONOTONE_HERMITE: *out = 2; return TRUE;
    default: return FALSE;
  }
}

static gboolean curve_predicate_holds(const dt_remote_parameter_predicate_t *predicate,
                                      const dt_introspection_field_t *root,
                                      void *params,
                                      gboolean *out,
                                      dt_remote_error_t **error)
{
  if(!predicate)
  {
    *out = TRUE;
    return TRUE;
  }

  const dt_remote_path_segment_t segment = {
    .type = DT_REMOTE_PATH_FIELD, .value.field = predicate->field,
  };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!dt_remote_path_resolve(&path, root, params, &field, &ptr, error)) return FALSE;
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_ENUM
     || (predicate->op != DT_REMOTE_PREDICATE_EQ && predicate->op != DT_REMOTE_PREDICATE_NE))
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: curve predicate '%s' drifted from introspection"),
                          predicate->field ? predicate->field : ""),
                        error);
    return FALSE;
  }

  const char *live_name = dt_introspection_get_enum_name((dt_introspection_field_t *)field,
                                                          *(const int *)ptr);
  if(!live_name)
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: curve predicate '%s' has an unknown enum value"),
                          predicate->field),
                        error);
    return FALSE;
  }
  const gboolean equal = !g_strcmp0(live_name, predicate->enum_name);
  *out = predicate->op == DT_REMOTE_PREDICATE_EQ ? equal : !equal;
  return TRUE;
}

static const dt_remote_curve_descriptor_t *find_curve_descriptor(
  const dt_remote_curve_module_adapter_t *adapter,
  const char *name)
{
  if(!adapter || !name) return NULL;
  for(guint i = 0; i < adapter->curve_count; i++)
    if(!g_strcmp0(adapter->curves[i].name, name)) return &adapter->curves[i];
  return NULL;
}

static gboolean patch_mentions_prepare_field(const dt_remote_curve_module_adapter_t *adapter,
                                             const dt_remote_patch_t *patch)
{
  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *entry = g_ptr_array_index(patch->scalar_values, i);
    for(guint j = 0; entry && j < adapter->prepare_field_count; j++)
      if(!g_strcmp0(entry->name, adapter->prepare_fields[j])) return TRUE;
  }
  return FALSE;
}

static gboolean resolve_curve_native(const dt_remote_curve_descriptor_t *desc,
                                     const dt_introspection_field_t *root,
                                     void *params,
                                     const dt_introspection_field_t **nodes_field,
                                     void **nodes_ptr,
                                     const dt_introspection_field_t **count_field,
                                     void **count_ptr,
                                     const dt_introspection_field_t **type_field,
                                     void **type_ptr,
                                     dt_remote_error_t **error)
{
  return dt_remote_path_resolve(&desc->native.nodes, root, params,
                                nodes_field, nodes_ptr, error)
         && dt_remote_path_resolve(&desc->native.count, root, params,
                                   count_field, count_ptr, error)
         && dt_remote_path_resolve(&desc->native.type, root, params,
                                   type_field, type_ptr, error);
}

static gboolean write_curve_patch(const dt_remote_curve_descriptor_t *desc,
                                  const dt_introspection_field_t *root,
                                  void *params,
                                  const dt_remote_curve_patch_t *curve,
                                  dt_remote_error_t **error)
{
  if(!dt_remote_curve_validate(desc, curve->points, curve->has_interpolation,
                               curve->interpolation, error))
    return FALSE;

  const dt_introspection_field_t *nodes_field = NULL;
  const dt_introspection_field_t *count_field = NULL;
  const dt_introspection_field_t *type_field = NULL;
  void *nodes_ptr = NULL;
  void *count_ptr = NULL;
  void *type_ptr = NULL;
  if(!resolve_curve_native(desc, root, params, &nodes_field, &nodes_ptr,
                           &count_field, &count_ptr, &type_field, &type_ptr, error))
    return FALSE;

  if(!nodes_field || nodes_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || !is_curve_integer_leaf(count_field->header.type)
     || !is_curve_integer_leaf(type_field->header.type))
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: curve '%s' native layout drifted"), desc->name),
                        error);
    return FALSE;
  }

  dt_remote_curve_interpolation_t interpolation = curve->interpolation;
  if(!curve->has_interpolation)
  {
    gint64 native_type = 0;
    if(!read_curve_integer_leaf(type_field, type_ptr, &native_type)
       || !map_native_curve_interpolation(native_type, &interpolation))
    {
      deliver_curve_error(dt_remote_curve_error_new(
                            DT_REMOTE_ERR_INTERNAL,
                            _("internal error: curve '%s' has an unknown native interpolation"),
                            desc->name),
                          error);
      return FALSE;
    }
    if(!dt_remote_curve_validate(desc, curve->points, TRUE, interpolation, error)) return FALSE;
  }

  const guint count = curve->points ? curve->points->len : 0;
  const guint capacity = (guint)nodes_field->Array.count;
  if(count > capacity)
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: curve '%s' exceeds native capacity"), desc->name),
                        error);
    return FALSE;
  }

  // rgbcurve padding is insignificant: deterministically clear every
  // unused native node instead of preserving stale memory.
  for(guint i = 0; i < capacity; i++)
  {
    dt_introspection_field_t *node_field = NULL;
    void *node_ptr = dt_introspection_access_array((dt_introspection_field_t *)nodes_field,
                                                    nodes_ptr, i, &node_field);
    dt_introspection_field_t *x_field = NULL;
    dt_introspection_field_t *y_field = NULL;
    void *x_ptr = node_ptr
      ? dt_introspection_get_child(node_field, node_ptr, desc->native.x_field, &x_field) : NULL;
    void *y_ptr = node_ptr
      ? dt_introspection_get_child(node_field, node_ptr, desc->native.y_field, &y_field) : NULL;
    if(!x_ptr || !y_ptr || !is_curve_floating_leaf(x_field->header.type)
       || !is_curve_floating_leaf(y_field->header.type))
    {
      deliver_curve_error(dt_remote_curve_error_new(
                            DT_REMOTE_ERR_INTERNAL,
                            _("internal error: curve '%s' node %u drifted"), desc->name, i),
                          error);
      return FALSE;
    }
    const double x = i < count
      ? g_array_index(curve->points, dt_remote_curve_point_t, i).x : 0.0;
    const double y = i < count
      ? g_array_index(curve->points, dt_remote_curve_point_t, i).y : 0.0;
    if(!write_curve_float_leaf(x_field, x_ptr, x) || !write_curve_float_leaf(y_field, y_ptr, y))
    {
      deliver_curve_error(dt_remote_curve_error_new(
                            DT_REMOTE_ERR_INTERNAL,
                            _("internal error: curve '%s' coordinate type drifted"), desc->name),
                          error);
      return FALSE;
    }
  }

  gint64 native_interpolation = 0;
  if(!map_remote_curve_interpolation(interpolation, &native_interpolation)
     || !write_curve_integer_leaf(count_field, count_ptr, count)
     || !write_curve_integer_leaf(type_field, type_ptr, native_interpolation))
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: curve '%s' integer layout drifted"), desc->name),
                        error);
    return FALSE;
  }

  if(desc->native.internal_version.length)
  {
    const dt_introspection_field_t *version_field = NULL;
    void *version_ptr = NULL;
    if(!dt_remote_path_resolve(&desc->native.internal_version, root, params,
                               &version_field, &version_ptr, error)
       || !write_curve_integer_leaf(version_field, version_ptr,
                                    desc->native.internal_version_value))
    {
      if(!error || !*error)
        deliver_curve_error(dt_remote_curve_error_new(
                              DT_REMOTE_ERR_INTERNAL,
                              _("internal error: curve '%s' version layout drifted"), desc->name),
                            error);
      return FALSE;
    }
  }
  return TRUE;
}

gboolean dt_remote_curve_apply_patch(const struct dt_iop_module_t *module,
                                     const void *old_params,
                                     void *new_params,
                                     const dt_remote_patch_t *patch,
                                     dt_remote_error_t **error)
{
  if(!module || !module->so || !old_params || !new_params || !patch)
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: null argument to curve mutation engine")), error);
    return FALSE;
  }

  gboolean has_curve_semantics = FALSE;
  const char *first_curve_name = NULL;
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(semantic && semantic->class_id == DT_REMOTE_PARAMETER_CURVE)
    {
      has_curve_semantics = TRUE;
      first_curve_name = semantic->value.curve.name;
      break;
    }
  }

  dt_introspection_t *intro = module->so->get_introspection
    ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_INTERNAL,
                          _("internal error: module '%s' has no curve introspection"), module->op),
                        error);
    return FALSE;
  }

  const dt_remote_curve_module_adapter_t *adapter =
    dt_remote_curve_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    if(!has_curve_semantics) return TRUE;
    deliver_curve_error(dt_remote_curve_error_new(
                          DT_REMOTE_ERR_UNKNOWN_FIELD,
                          _("unknown semantic curve '%s'"),
                          first_curve_name ? first_curve_name : ""),
                        error);
    return FALSE;
  }

  const gboolean prepare_needed = has_curve_semantics || patch_mentions_prepare_field(adapter, patch);
  // A scalar-only patch unrelated to adapter preparation must remain
  // independent of semantic registry health. Registry drift disables curve
  // support for this operation, not its primitive field support.
  if(!prepare_needed) return TRUE;
  if(!dt_remote_curve_registry_validate(adapter, intro, error)) return FALSE;

  const dt_remote_curve_context_t context = {
    .module = module, .introspection = intro, .adapter = adapter,
  };
  if(prepare_needed && adapter->prepare
     && !adapter->prepare(&context, old_params, new_params, patch, error))
    return FALSE;

  // Resolve every curve-class request ID before the registry-ordered write
  // loop, skipping every non-curve entry entirely -- the same request may
  // carry vector entries the vector engine handles separately
  // (remote_vector.c).
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(!semantic)
    {
      deliver_curve_error(dt_remote_curve_error_new(
                            DT_REMOTE_ERR_INTERNAL,
                            _("internal error: null semantic patch entry")), error);
      return FALSE;
    }
    if(semantic->class_id != DT_REMOTE_PARAMETER_CURVE) continue;
    const char *name = semantic->value.curve.name;
    if(!find_curve_descriptor(adapter, name))
    {
      deliver_curve_error(dt_remote_curve_error_new(
                            DT_REMOTE_ERR_UNKNOWN_FIELD,
                            _("unknown semantic curve '%s'"), name ? name : ""), error);
      return FALSE;
    }
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_semantic_patch_t *prior = g_ptr_array_index(patch->semantic_values, j);
      if(prior && prior->class_id == DT_REMOTE_PARAMETER_CURVE
         && !g_strcmp0(prior->value.curve.name, name))
      {
        deliver_curve_error(curve_parameter_error_new(
                              DT_REMOTE_ERR_INVALID_VALUE, name, "duplicate_parameter",
                              _("duplicate semantic curve '%s'"), name), error);
        return FALSE;
      }
    }
  }

  // Apply in descriptor/registry order, never request hash/array order.
  for(guint descriptor_index = 0; descriptor_index < adapter->curve_count; descriptor_index++)
  {
    const dt_remote_curve_descriptor_t *desc = &adapter->curves[descriptor_index];
    const dt_remote_curve_patch_t *curve = NULL;
    for(guint patch_index = 0;
        patch->semantic_values && patch_index < patch->semantic_values->len;
        patch_index++)
    {
      const dt_remote_semantic_patch_t *semantic =
        g_ptr_array_index(patch->semantic_values, patch_index);
      if(!g_strcmp0(semantic->value.curve.name, desc->name))
      {
        curve = &semantic->value.curve;
        break;
      }
    }
    if(!curve) continue;

    gboolean active = TRUE;
    gboolean writable = TRUE;
    if(!curve_predicate_holds(desc->active_when, intro->field, new_params, &active, error)
       || !curve_predicate_holds(desc->writable_when, intro->field, new_params, &writable, error))
      return FALSE;
    if(!active || !writable)
    {
      deliver_curve_error(curve_parameter_error_new(
                            DT_REMOTE_ERR_UNSUPPORTED_FIELD, desc->name,
                            "condition_not_satisfied",
                            _("semantic curve '%s' is inactive or not writable in projected state"),
                            desc->name),
                          error);
      return FALSE;
    }

    if(!write_curve_patch(desc, intro->field, new_params, curve, error)) return FALSE;
  }

  if(prepare_needed && adapter->validate_completed
     && !adapter->validate_completed(&context, new_params, error))
    return FALSE;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
