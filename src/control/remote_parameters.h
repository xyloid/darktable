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

// Neutral semantic (non-scalar) parameter types for the darktable MCP
// remote-edit sidecar -- curve schemas/values/patches and the shared
// class/writability vocabulary they build on. See the curve-classes
// design doc §Neutral semantic types for the normative definitions this
// header copies verbatim.
//
// This header is pure data plus paired constructors/destructors: no
// introspection, no registry, no JSON. Later steps (the curve
// introspection cursor, validator, per-op registry, and the JSON
// request/response mapping) are built on top of these types but are out
// of scope here.
//
// Threading: like remote_edit.h, any future code that walks a live
// darkroom module to produce or apply these types must do so from the
// GTK main thread; the types themselves are inert data with no such
// constraint.

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* common class and status                                                 */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_parameter_class_t
{
  DT_REMOTE_PARAMETER_CURVE = 1,
  DT_REMOTE_PARAMETER_BANDS,               // fixed-count band samples, milestone 5
  DT_REMOTE_PARAMETER_VECTOR              // replaces DT_REMOTE_PARAMETER_LEVELS
} dt_remote_parameter_class_t;

typedef enum dt_remote_writability_t
{
  DT_REMOTE_WRITABLE_NEVER = 0,
  DT_REMOTE_WRITABLE_NOW,
  DT_REMOTE_WRITABLE_CONDITIONAL
} dt_remote_writability_t;

// The class enum is internal and extensible. Serialized class names are
// stable strings and do not expose these integer values.

/* ---------------------------------------------------------------------- */
/* curve schema                                                            */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_curve_interpolation_t
{
  DT_REMOTE_CURVE_CUBIC_SPLINE = 0,
  DT_REMOTE_CURVE_CATMULL_ROM,
  DT_REMOTE_CURVE_MONOTONE_HERMITE
} dt_remote_curve_interpolation_t;

typedef enum dt_remote_curve_endpoint_policy_t
{
  DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL = 0,
  DT_REMOTE_CURVE_BOUNDARY_POINTS_REQUIRED,
  DT_REMOTE_CURVE_BOUNDARY_POINTS_FIXED_IDENTITY
} dt_remote_curve_endpoint_policy_t;

typedef enum dt_remote_spacing_rule_t
{
  DT_REMOTE_SPACING_NONE = 0,
  DT_REMOTE_SPACING_AT_LEAST,
  DT_REMOTE_SPACING_GREATER_THAN
} dt_remote_spacing_rule_t;

typedef enum dt_remote_predicate_operator_t
{
  DT_REMOTE_PREDICATE_EQ = 0,
  DT_REMOTE_PREDICATE_NE
} dt_remote_predicate_operator_t;

typedef struct dt_remote_parameter_condition_t
{
  char *field;                // owned stable primitive field name
  dt_remote_predicate_operator_t op;
  char *enum_name;            // owned stable enum name
} dt_remote_parameter_condition_t;

typedef struct dt_remote_curve_axis_t
{
  double minimum;
  double maximum;
  char *unit;                 // owned, nullable; v1 normally "normalized"
} dt_remote_curve_axis_t;

typedef struct dt_remote_curve_schema_t
{
  char *name;                // stable semantic ID, for example "curve.master"
  char *display_name;        // translated presentation label
  char *description;         // translated, nullable
  dt_remote_curve_axis_t x;
  dt_remote_curve_axis_t y;
  guint minimum_points;
  guint maximum_points;
  double minimum_x_spacing;
  dt_remote_spacing_rule_t adjacent_spacing_rule;
  double minimum_wrap_spacing;
  dt_remote_spacing_rule_t wrap_spacing_rule;
  gboolean strict_x_order;
  gboolean periodic_x;
  dt_remote_curve_endpoint_policy_t boundary_point_policy;
  guint interpolation_mask;
  dt_remote_curve_interpolation_t default_interpolation;
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;   // owned, nullable
  dt_remote_parameter_condition_t *writable_when; // owned, nullable
  dt_remote_parameter_condition_t *periodic_when; // owned, nullable
} dt_remote_curve_schema_t;

// Returned schemas own every string and use paired destroy functions.
// Registry descriptors use static strings; conversion copies them into
// response objects.
//
// `periodic_x` means unconditionally periodic. `periodic_when` represents
// the conditional case. Conditions are structured field/operator/enum
// triples, not free-form expressions. A value read from a live instance
// carries the resolved Boolean.

/** frees a curve schema, including its axes' units, its three optional
 * conditions, and the schema struct itself. NULL-safe. */
void dt_remote_curve_schema_free(dt_remote_curve_schema_t *schema);

/* ---------------------------------------------------------------------- */
/* curve value                                                             */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_curve_point_t
{
  double x;
  double y;
} dt_remote_curve_point_t;

typedef struct dt_remote_curve_value_t
{
  char *name;                // owned semantic ID
  GArray *points;            // dt_remote_curve_point_t, owned
  dt_remote_curve_interpolation_t interpolation;
  gboolean active;
  gboolean effective;
  gboolean writable_now;
  gboolean periodic_x;
} dt_remote_curve_value_t;

// `active` means the semantic curve exists in the current mode.
// `effective` means processing currently consumes it. `writable_now` is
// resolved against the projected params block and may differ from the
// per-op schema's general capability.

/** frees a curve value, including its owned name and points array, and
 * the value struct itself. NULL-safe. */
void dt_remote_curve_value_free(dt_remote_curve_value_t *value);

/* ---------------------------------------------------------------------- */
/* vector schema                                                           */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_vector_subtype_t
{
  DT_REMOTE_VECTOR_PLAIN = 0,
  DT_REMOTE_VECTOR_COLOR,
  DT_REMOTE_VECTOR_LEVELS
} dt_remote_vector_subtype_t;

typedef struct dt_remote_vector_component_schema_t
{
  char *name;                 // owned
  double minimum;
  double maximum;
} dt_remote_vector_component_schema_t;

typedef struct dt_remote_vector_schema_t
{
  char *name;                 // stable semantic ID, for example "vector.wb_coeffs"
  char *display_name;         // presentation label
  char *description;          // nullable
  dt_remote_vector_subtype_t subtype;
  char *color_space;          // owned, nullable; non-NULL iff subtype COLOR ("display_rgb")
  GArray *components;         // dt_remote_vector_component_schema_t, owned (owned names)
  gboolean strictly_increasing; // subtype LEVELS ordering
  double minimum_gap;         // absolute, at-least; 0.0 = none; LEVELS only
  dt_remote_writability_t writability;
  dt_remote_parameter_condition_t *active_when;   // owned, nullable
  dt_remote_parameter_condition_t *writable_when; // owned, nullable
} dt_remote_vector_schema_t;

/** frees a vector schema, including its components' owned names, its two
 * optional conditions, and the schema struct itself. NULL-safe. */
void dt_remote_vector_schema_free(dt_remote_vector_schema_t *schema);

/* ---------------------------------------------------------------------- */
/* vector value                                                            */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_vector_value_t
{
  char *name;                 // owned semantic ID
  GArray *values;              // double, owned
  gboolean active;
  gboolean effective;
  gboolean writable_now;
} dt_remote_vector_value_t;

// `active` means the semantic vector exists in the current mode.
// `effective` means processing currently consumes it. `writable_now` is
// resolved against the projected params block and may differ from the
// per-op schema's general capability -- same conventions as the curve
// value type above.

/** frees a vector value, including its owned name and values array, and
 * the value struct itself. NULL-safe. */
void dt_remote_vector_value_free(dt_remote_vector_value_t *value);

/* ---------------------------------------------------------------------- */
/* semantic patch                                                           */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_curve_patch_t
{
  char *name;
  GArray *points;
  gboolean has_interpolation;
  dt_remote_curve_interpolation_t interpolation;
} dt_remote_curve_patch_t;

typedef struct dt_remote_vector_patch_t
{
  char *name;        // owned semantic ID
  GArray *values;    // double elements, owned
} dt_remote_vector_patch_t;

typedef struct dt_remote_band_patch_t
{
  char *name;      // owned semantic ID
  GArray *y;       // double elements, owned, required
  GArray *x;       // double elements, owned, NULL when absent
} dt_remote_band_patch_t;

typedef struct dt_remote_semantic_patch_t
{
  dt_remote_parameter_class_t class_id;
  union
  {
    dt_remote_curve_patch_t curve;
    dt_remote_vector_patch_t vector;
    dt_remote_band_patch_t bands;
  } value;
} dt_remote_semantic_patch_t;

// The request parser owns and frees patches. It rejects unknown members,
// duplicate semantic IDs, oversized point/component/sample lists, and
// non-numeric/non-finite JSON before invoking `remote_edit`.

/** frees a single semantic patch entry (its class-specific owned
 * members -- name and points for DT_REMOTE_PARAMETER_CURVE, name and
 * values for DT_REMOTE_PARAMETER_VECTOR, name and y (and x, if present)
 * for DT_REMOTE_PARAMETER_BANDS -- and the patch struct itself).
 * Suitable as a GDestroyNotify for the `dt_remote_patch_t.semantic_values`
 * GPtrArray. NULL-safe. */
void dt_remote_semantic_patch_free(gpointer patch_ptr);

/* ---------------------------------------------------------------------- */
/* tagged semantic schema/value wrappers                                    */
/* ---------------------------------------------------------------------- */

// Class-tagged containers for semantic schemas/values as they travel
// through dt_remote_module_schema_t.semantic_fields and
// dt_remote_mutation_result_t.semantic_values (remote_edit.h). The curve
// engine keeps producing plain curve types; producers wrap at the
// insertion sites and the JSON layer unwraps by switching on `class_id`.
// Purely internal shape -- the wire is unchanged.

typedef struct dt_remote_semantic_schema_t
{
  dt_remote_parameter_class_t class_id;
  union { dt_remote_curve_schema_t *curve; dt_remote_vector_schema_t *vector; } u;   // owned
} dt_remote_semantic_schema_t;

typedef struct dt_remote_semantic_value_t
{
  dt_remote_parameter_class_t class_id;
  union { dt_remote_curve_value_t *curve; dt_remote_vector_value_t *vector; } u;    // owned
} dt_remote_semantic_value_t;

/** wraps an owned curve schema in a DT_REMOTE_PARAMETER_CURVE-tagged
 * wrapper, taking ownership of `s`. */
dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_curve(dt_remote_curve_schema_t *s);

/** wraps an owned curve value in a DT_REMOTE_PARAMETER_CURVE-tagged
 * wrapper, taking ownership of `v`. */
dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_curve(dt_remote_curve_value_t *v);

/** wraps an owned vector schema in a DT_REMOTE_PARAMETER_VECTOR-tagged
 * wrapper, taking ownership of `s`. */
dt_remote_semantic_schema_t *dt_remote_semantic_schema_wrap_vector(dt_remote_vector_schema_t *s);

/** wraps an owned vector value in a DT_REMOTE_PARAMETER_VECTOR-tagged
 * wrapper, taking ownership of `v`. */
dt_remote_semantic_value_t *dt_remote_semantic_value_wrap_vector(dt_remote_vector_value_t *v);

/** frees a tagged semantic schema wrapper and its class-specific owned
 * payload. GDestroyNotify-able. NULL-safe. */
void dt_remote_semantic_schema_free(gpointer schema_ptr);

/** frees a tagged semantic value wrapper and its class-specific owned
 * payload. GDestroyNotify-able. NULL-safe. */
void dt_remote_semantic_value_free(gpointer value_ptr);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
