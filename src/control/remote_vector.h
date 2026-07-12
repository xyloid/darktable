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
// semantic-vector machinery: the registry descriptor types, the module
// adapter/context shapes, and the registry/validator/apply API that build
// on them. This mirrors the curve-class split (remote_curve.h/.c +
// remote_curve_registry.c) one-for-one: a vector descriptor's native
// layout is simpler than a curve's -- a single fixed-capacity float array
// leaf, resolved through dt_remote_path_resolve() (remote_curve.h), never
// a struct-of-nodes array -- so there is no vector-specific path cursor
// here, only the pieces that differ from the curve twin.
//
// Neutral (non-introspection) vector schema/value/patch types live in
// control/remote_parameters.h, not here -- see that header's comment for
// the file split rationale (same reasoning as remote_curve.h's own
// comment).
//
// Threading: like remote_curve.h/remote_edit.h, any code that resolves a
// path against a live darkroom module's params block must do so from the
// GTK main thread; the descriptor/adapter types themselves are inert
// compiled-in data with no such constraint.

#pragma once

#include "common/introspection.h"
#include "control/remote_curve.h" // dt_remote_introspection_path_t, dt_remote_path_resolve(),
                                  // dt_remote_parameter_predicate_t
#include "control/remote_edit.h" // dt_remote_error_t, dt_remote_patch_t
#include "control/remote_parameters.h" // dt_remote_vector_subtype_t

#include <glib.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* registry descriptor types                                              */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_vector_component_t
{
  const char *name;
  double minimum;
  double maximum;
} dt_remote_vector_component_t;

typedef struct dt_remote_vector_descriptor_t
{
  const char *name;               // stable semantic ID
  const char *display_name;
  const char *description;
  dt_remote_introspection_path_t native;  // -> float array leaf (FIELD [+ INDEX row])
  guint component_count;                  // exposed on the wire
  const dt_remote_vector_component_t *components;  // component_count entries
  guint native_capacity;                  // full array length; [component_count..) preserved
  dt_remote_vector_subtype_t subtype;
  const char *color_space;                // non-NULL iff subtype COLOR ("display_rgb")
  gboolean strictly_increasing;           // subtype LEVELS ordering
  double minimum_gap;                     // absolute, at-least; LEVELS only
  const dt_remote_parameter_predicate_t *active_when;    // same type curve descriptors use
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_vector_descriptor_t;

/* ---------------------------------------------------------------------- */
/* module adapter                                                          */
/* ---------------------------------------------------------------------- */

// Forward-declared here so the validate_completed callback pointer below
// type-checks; the body follows the adapter declaration -- same idiom as
// remote_curve.h's dt_remote_curve_context_t forward reference.
struct dt_remote_vector_context_t;

struct dt_iop_module_t; // develop/imageop.h; kept a bare forward reference,
                        // same rationale as remote_curve.h's own.

typedef struct dt_remote_vector_module_adapter_t
{
  const char *operation;
  guint minimum_params_version;
  guint maximum_params_version;
  const dt_remote_vector_descriptor_t *vectors;
  guint vector_count;
  const char *const *prepare_fields;
  guint prepare_field_count;

  // Unlike the curve adapter, there is no `prepare` callback in v1: no
  // shipped vector adapter needs to project one component's write onto
  // another before the per-descriptor loop runs. `prepare_fields` still
  // exists so a scalar-only patch that touches a field a vector's
  // active_when/writable_when depends on still triggers
  // validate_completed() (mirrors the curve engine's prepare_needed gate).
  gboolean (*validate_completed)(const struct dt_remote_vector_context_t *ctx,
                                 const void *new_params,
                                 dt_remote_error_t **error);
} dt_remote_vector_module_adapter_t;

/** Per-call adapter context. All pointers are borrowed for the duration of
 * dt_remote_vector_apply_patch(); callbacks must not retain them. */
typedef struct dt_remote_vector_context_t
{
  const struct dt_iop_module_t *module;
  const dt_introspection_t *introspection;
  const dt_remote_vector_module_adapter_t *adapter;
} dt_remote_vector_context_t;

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

/** looks up the adapter registered for the stable IOP operation name
 * `operation` at `params_version`, or %NULL if no adapter covers that
 * (operation, params_version) pair -- including when `operation` is not a
 * vector-bearing op at all. Native array fields on such ops remain
 * unsupported and no semantic vector is advertised; this is not an
 * error. */
const dt_remote_vector_module_adapter_t *
dt_remote_vector_registry_lookup(const char *operation, guint params_version);

/* Test-only/internal seam for exercising semantic reads with synthetic
 * adapters through the public list_schema/read_values/apply_patch APIs,
 * mirroring dt_remote_curve_registry_set_lookup_override() (remote_curve.h)
 * exactly -- same rationale: the production registry table starts empty in
 * this task and is populated by later tasks, so the engine must be provable
 * against a hand-rolled adapter/real module .so pair before any adapter
 * ships. The override is process-wide and not thread-safe; tests must
 * install it only from a single-threaded process and reset it after each
 * test. Pass %NULL to restore the production registry lookup. Production
 * code never sets an override. */
typedef const dt_remote_vector_module_adapter_t *(*dt_remote_vector_registry_lookup_override_t)(
  const char *operation, guint params_version);
void dt_remote_vector_registry_set_lookup_override(dt_remote_vector_registry_lookup_override_t lookup);

/** validates that `adapter`'s nonempty operation matches `so`, and that the
 * nonnegative introspection params version is within its ordered inclusive
 * version range; that positive vector/prepare-field counts have non-NULL
 * backing pointers and every prepare-field ID is nonempty; and, for every
 * descriptor, that its ID and all component IDs are nonempty, its positive
 * component count has a non-NULL component array, and every component has
 * finite, ordered minimum/maximum bounds. Each descriptor subtype is checked
 * exhaustively: PLAIN permits no color space or ordering metadata; COLOR
 * requires a nonempty color space and permits no ordering metadata; LEVELS
 * requires strictly-increasing ordering, a finite nonnegative minimum gap,
 * and no color space; unknown subtype values are rejected. It then validates
 * that each native path resolves against `so`'s real introspection tree to an
 * array leaf whose declared and non-NULL element descriptor are both
 * `sizeof(float)` floats, of length >= component_count, with native_capacity
 * equal to that resolved array length exactly; that no two descriptors on
 * `adapter` share a name;
 * that no descriptor's name collides with a curve semantic ID registered for
 * the same (operation, params_version) pair (queried through
 * dt_remote_curve_registry_lookup()). Invalid metadata fails closed before
 * its pointer is traversed. One aggregate result is cached for the process
 * lifetime, keyed by (`adapter` identity, introspection's params_version) --
 * same caching convention as dt_remote_curve_registry_validate().
 *
 * Returns TRUE iff every check above passes. Returns FALSE, with `*error`
 * set to a newly allocated DT_REMOTE_ERR_INTERNAL (caller frees with
 * dt_remote_error_free()), otherwise -- this is always a registry/
 * introspection drift bug, never caller error. */
gboolean dt_remote_vector_registry_validate(const dt_remote_vector_module_adapter_t *adapter,
                                            const struct dt_iop_module_so_t *so,
                                            dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* vector engine API                                                       */
/* ---------------------------------------------------------------------- */

/** Per (operation, params_version) schema listing: looks up the adapter for
 * `so`'s operation/params_version, validates its registry shape
 * (dt_remote_vector_registry_validate() above, cached), then returns one
 * owned dt_remote_vector_schema_t per descriptor, in registry order
 * (deep-copying every string -- the descriptor's own strings are static and
 * outlive nothing past this call). An operation/version with no registered
 * adapter succeeds with `*out_fields` set to %NULL (silent degrade, same
 * spirit as the curve registry's no-adapter path, but NULL rather than an
 * empty array here). Registry validation failure instead fails closed: no
 * partial descriptor list is returned, `*out_fields` remains %NULL, and
 * `*error` is DT_REMOTE_ERR_INTERNAL. On success with at least one
 * descriptor, sets `*out_fields` to a newly allocated GPtrArray (owned by
 * the caller, element destructor already set to dt_remote_vector_schema_free
 * -- free with g_ptr_array_unref()). A descriptor without writable_when is
 * unconditionally DT_REMOTE_WRITABLE_NOW; one with a predicate is
 * DT_REMOTE_WRITABLE_CONDITIONAL. Null arguments and missing introspection
 * also fail with DT_REMOTE_ERR_INTERNAL and a %NULL output. */
gboolean dt_remote_vector_list_schema(const struct dt_iop_module_so_t *so,
                                      GPtrArray **out_fields,      /* dt_remote_vector_schema_t* */
                                      dt_remote_error_t **error);

/** Per live-instance value read: looks up + validates the adapter for
 * `module`'s operation/params_version (as above), then for every descriptor
 * evaluates active_when/writable_when against `params` (not necessarily
 * `module->params` -- callers may pass a projected/candidate block) and
 * reads the live float components through dt_remote_path_resolve(),
 * widening each to a double. An operation/version with no registered
 * adapter succeeds with an empty table. Registry validation failure,
 * predicate/path resolution failure fails the whole read with
 * DT_REMOTE_ERR_INTERNAL: any temporary values are freed and `*out` remains
 * %NULL, never a partial result. On success, sets `*out` to a newly
 * allocated GHashTable (name -> owned dt_remote_vector_value_t, owned by
 * the caller -- free with g_hash_table_unref(), value destructor already
 * set to dt_remote_vector_value_free()). Null arguments and missing
 * introspection also fail with DT_REMOTE_ERR_INTERNAL and a %NULL
 * output. */
gboolean dt_remote_vector_read_values(const struct dt_iop_module_t *module,
                                      const void *params,
                                      GHashTable **out,            /* name -> dt_remote_vector_value_t */
                                      dt_remote_error_t **error);

/** Applies the vector-class portion of `patch` to the caller-owned
 * projected params block. Scalar entries must already have been written to
 * `new_params`; `old_params` remains the pre-transaction block. Every entry
 * in `patch->semantic_values` whose class_id is not DT_REMOTE_PARAMETER_VECTOR
 * is skipped -- this engine only ever touches its own class, the same
 * request may carry curve entries the curve engine handles separately.
 * Predicate evaluation, vector validation/native writes, and
 * validate_completed all operate only on `new_params`. */
gboolean dt_remote_vector_apply_patch(const struct dt_iop_module_t *module,
                                      const void *old_params,
                                      void *new_params,
                                      const dt_remote_patch_t *patch,
                                      dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* common vector validator                                                 */
/* ---------------------------------------------------------------------- */

/** Validates a candidate vector patch (`values`, one double per component)
 * against `desc`:
 *
 *   - `values->len` must equal `desc->component_count` ("count_mismatch");
 *   - every component must be finite ("non_finite");
 *   - each finite component must fall within its own [minimum, maximum]
 *     domain, inclusive ("domain") -- compared entirely in double precision,
 *     so a value like 2.00000001 against a maximum of 2.0 is correctly
 *     rejected even though both would round to the same float;
 *   - when `desc->subtype` is DT_REMOTE_VECTOR_LEVELS, each adjacent pair
 *     must be strictly increasing ("unordered" when the delta is <= 0) and
 *     the gap must be at least `desc->minimum_gap` ("gap" when the delta is
 *     strictly less -- a delta exactly equal to minimum_gap is accepted).
 *
 * Every rejection returns FALSE with `*error` set to a newly allocated
 * DT_REMOTE_ERR_INVALID_VALUE (caller frees with dt_remote_error_free());
 * `error->details_json` is a serialized JSON object with a "parameter"
 * member (`desc->name`), a "constraint" member (one of the short stable
 * strings named above), and a "component_index" member (0-based index into
 * `values`) whenever the violation is component-specific -- omitted for the
 * whole-array "count_mismatch" failure. `values` is never modified, on any
 * path, including rejection.
 *
 * Pure function: no introspection calls, no registry lookups, no JSON
 * parsing of the input. */
gboolean dt_remote_vector_validate(const dt_remote_vector_descriptor_t *desc,
                                   const GArray *values, /* double */
                                   dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
