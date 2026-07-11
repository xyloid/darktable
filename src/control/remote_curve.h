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
#include "control/remote_parameters.h" // dt_remote_curve_interpolation_t, dt_remote_curve_endpoint_policy_t,
                                       // dt_remote_spacing_rule_t, dt_remote_predicate_operator_t,
                                       // dt_remote_curve_point_t

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

/* ---------------------------------------------------------------------- */
/* registry descriptor types (curve-classes design doc                    */
/* SS Registry descriptors: "Native control-point layout", "Predicate",   */
/* "Semantic descriptor" -- copied verbatim). Declared here because they  */
/* are the input dt_remote_curve_validate() below consumes; nothing in    */
/* this task constructs, resolves, or introspects them yet (that is Task  */
/* 6's job for descriptor instances and Task 8's for the module adapter/  */
/* registry that walks them). dt_remote_curve_module_adapter_t (the       */
/* sibling type declared right after the semantic descriptor in the       */
/* design doc, with its `prepare`/`validate_completed` callbacks and a    */
/* forward reference to `struct dt_remote_curve_context_t`) is deliberately */
/* NOT declared here -- it is out of scope until the task that defines    */
/* dt_remote_curve_context_t.                                             */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_native_curve_layout_t
{
  dt_remote_introspection_path_t nodes; // fixed-capacity node array
  dt_remote_introspection_path_t count; // active point count leaf
  dt_remote_introspection_path_t type;  // interpolation leaf
  const char *x_field;                  // node member, normally "x"
  const char *y_field;                  // node member, normally "y"
  dt_remote_introspection_path_t internal_version; // length 0 if absent
  gint64 internal_version_value;
} dt_remote_native_curve_layout_t;

typedef struct dt_remote_parameter_predicate_t
{
  const char *field;
  dt_remote_predicate_operator_t op;
  const char *enum_name;
} dt_remote_parameter_predicate_t;

// Initial predicates compare one primitive enum field with one stable enum
// name. This is sufficient for rgbcurve and tonecurve. Predicates resolve
// enum names through introspection; descriptors do not hard-code native
// integers. Not a general expression language -- more complex state uses
// an adapter callback (dt_remote_curve_module_adapter_t, out of scope
// here).

typedef struct dt_remote_curve_axis_descriptor_t
{
  double minimum;
  double maximum;
  const char *unit;
} dt_remote_curve_axis_descriptor_t;

typedef struct dt_remote_curve_descriptor_t
{
  const char *name;
  const char *display_name_msgid;
  const char *description_msgid;
  dt_remote_native_curve_layout_t native;
  dt_remote_curve_axis_descriptor_t x;
  dt_remote_curve_axis_descriptor_t y;
  guint minimum_points;
  guint maximum_points;
  double minimum_x_spacing;
  dt_remote_spacing_rule_t adjacent_spacing_rule;
  double minimum_wrap_spacing;
  dt_remote_spacing_rule_t wrap_spacing_rule;
  gboolean strict_x_order;
  dt_remote_curve_endpoint_policy_t boundary_point_policy;
  guint interpolation_mask;
  dt_remote_curve_interpolation_t default_interpolation;
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
  const dt_remote_parameter_predicate_t *periodic_when;
} dt_remote_curve_descriptor_t;

// `interpolation_mask` is a bitmask of allowed dt_remote_curve_interpolation_t
// values, one bit per value: `(1u << DT_REMOTE_CURVE_CUBIC_SPLINE) |
// (1u << DT_REMOTE_CATMULL_ROM) | ...`. No existing `_mask` bitfield
// precedent was found elsewhere in src/control/remote_edit.c or
// remote_protocol.c to match; this is a fresh, straightforward convention.

/* ---------------------------------------------------------------------- */
/* common curve validator (curve-classes design doc SS Validation         */
/* algorithm, items 3-10 only)                                            */
/* ---------------------------------------------------------------------- */

/** Validates a candidate curve patch (`points`, and an optional new
 * `interpolation`) against `desc`, implementing ONLY items 3-10 of the
 * design doc's "Validation algorithm" list:
 *
 *   3. point count against descriptor bounds (minimum_points/maximum_points;
 *      global request-size limits are a protocol-layer concern, not this
 *      function's);
 *   4. reject NaN/infinity ("non_finite") or values outside the x/y domain
 *      ("domain_x"/"domain_y");
 *   5. strict ascending x when `desc->strict_x_order` is set -- a repeated
 *      x is reported as "duplicate_x", a decrease as "strict_order";
 *   6. adjacent minimum spacing ("adjacent_spacing"), skipped entirely when
 *      `desc->adjacent_spacing_rule == DT_REMOTE_SPACING_NONE`, otherwise
 *      compared with `>=` (AT_LEAST) or `>` (GREATER_THAN) against
 *      `desc->minimum_x_spacing`;
 *   7. periodic wrap spacing ("wrap_spacing") between the last and first
 *      point across the domain wrap, skipped entirely when
 *      `desc->wrap_spacing_rule == DT_REMOTE_SPACING_NONE`, otherwise
 *      compared the same way against `desc->minimum_wrap_spacing`; the wrap
 *      gap is `(first.x - x.minimum) + (x.maximum - last.x)`;
 *   8. the domain-boundary point policy ("boundary_policy"):
 *      BOUNDARY_POINTS_OPTIONAL checks nothing; BOUNDARY_POINTS_REQUIRED
 *      requires first.x == x.minimum and last.x == x.maximum (y
 *      unconstrained); BOUNDARY_POINTS_FIXED_IDENTITY additionally requires
 *      first.y == y.minimum and last.y == y.maximum;
 *   9. interpolation resolution -- only the "was one supplied?" half:
 *      when `has_interpolation` is FALSE, no interpolation check runs here
 *      (resolving/preserving the *current* interpolation needs native
 *      introspection and is Task 8's job);
 *  10. when `has_interpolation` is TRUE, `interpolation` must be a set bit
 *      of `desc->interpolation_mask` ("interpolation_not_allowed").
 *
 * Items 1-2 (semantic-ID resolution, active/writable predicate evaluation)
 * and 11-14 (native introspection reads/writes, adapter validate_completed)
 * are explicitly NOT implemented here -- see Task 8.
 *
 * Every rejection returns FALSE with `*error` set to a newly allocated
 * DT_REMOTE_ERR_INVALID_VALUE (caller frees with dt_remote_error_free());
 * `error->details_json` is a serialized JSON object with a "parameter"
 * member (`desc->name`), a "constraint" member (one of the short stable
 * strings named above), and a "point_index" member (0-based index into
 * `points`) whenever the violation is point-specific -- omitted for
 * whole-array failures (min_points/max_points) and for
 * interpolation_not_allowed. `points` is never modified, on any path,
 * including rejection: no clamping, sorting, deduplication, or implicit
 * boundary-point insertion.
 *
 * Pure function: no introspection calls, no registry lookups, no JSON
 * parsing of the input. */
gboolean dt_remote_curve_validate(const dt_remote_curve_descriptor_t *desc,
                                  const GArray *points, /* dt_remote_curve_point_t */
                                  gboolean has_interpolation,
                                  dt_remote_curve_interpolation_t interpolation,
                                  dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
