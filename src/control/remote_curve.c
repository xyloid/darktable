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

#include <limits.h>
#include <math.h>
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
   * Task 8's job (the other half of item 9); this function only checks
   * the allowlist, and only when the caller actually supplied a value. */
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
