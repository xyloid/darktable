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
// semantic-band machinery: the registry descriptor types, the module
// adapter/context shapes, and the registry/validator/apply API that build
// on them. This mirrors the vector-class split (remote_vector.h/.c +
// remote_vector_registry.c) one-for-one -- a band-set's native layout is
// two independent fixed-capacity float array leaves (`native_x`,
// `native_y`), each resolved through dt_remote_path_resolve()
// (remote_curve.h), never a struct-of-nodes array, exactly like a vector's
// single native leaf -- so there is no band-specific path cursor here,
// only the pieces that differ from the vector twin: two native leaves
// instead of one, a fixed count instead of a declared component list, an
// x-writability policy (FIXED/INTERIOR) instead of a subtype, and optional
// twin-channel x sharing.
//
// Neutral (non-introspection) band schema/value/patch types live in
// control/remote_parameters.h, not here -- see that header's comment for
// the file split rationale (same reasoning as remote_vector.h's own
// comment). `dt_remote_band_x_policy_t` in particular lives in
// remote_parameters.h rather than here for the same circular-include
// reason `dt_remote_vector_subtype_t` does (see that type's note in
// remote_vector.h and in remote_parameters.h).
//
// Threading: like remote_vector.h/remote_curve.h/remote_edit.h, any code
// that resolves a path against a live darkroom module's params block must
// do so from the GTK main thread; the descriptor/adapter types themselves
// are inert compiled-in data with no such constraint.

#pragma once

#include "common/introspection.h"
#include "control/remote_curve.h" // dt_remote_introspection_path_t, dt_remote_path_resolve(),
                                  // dt_remote_parameter_predicate_t
#include "control/remote_edit.h" // dt_remote_error_t, dt_remote_patch_t
#include "control/remote_parameters.h" // dt_remote_band_x_policy_t

#include <glib.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* registry descriptor types                                              */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_band_descriptor_t
{
  const char *name;               // stable semantic ID, e.g. "bands.luma"
  const char *display_name;
  const char *description;
  dt_remote_introspection_path_t native_x;  // -> float array leaf (FIELD [+ INDEX row])
  dt_remote_introspection_path_t native_y;  // same shape as native_x
  guint count;                              // fixed band count N
  double y_minimum, y_maximum;              // [0,1] for all v1 adapters
  dt_remote_band_x_policy_t x_policy;
  double minimum_gap;                       // absolute, at-least; INTERIOR only (0.001)
  const char *x_shared_with;                // twin semantic ID, NULL if none
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_band_descriptor_t;

/* ---------------------------------------------------------------------- */
/* module adapter                                                          */
/* ---------------------------------------------------------------------- */

// Forward-declared here so the validate_completed callback pointer below
// type-checks; the body follows the adapter declaration -- same idiom as
// remote_vector.h's dt_remote_vector_context_t forward reference.
struct dt_remote_band_context_t;

struct dt_iop_module_t; // develop/imageop.h; kept a bare forward reference,
                        // same rationale as remote_vector.h's own.

typedef struct dt_remote_band_module_adapter_t
{
  const char *operation;
  guint minimum_params_version;
  guint maximum_params_version;
  const dt_remote_band_descriptor_t *bands;
  guint band_count;
  const char *const *prepare_fields;
  guint prepare_field_count;

  // No `prepare` callback in v1, same rationale as the vector adapter:
  // no shipped band adapter needs to project one band's write onto
  // another before the per-descriptor loop runs (twin x-sharing is
  // handled by the engine itself, not an adapter callback).
  // `prepare_fields` still exists so a scalar-only patch that touches a
  // field a band's active_when/writable_when depends on still triggers
  // validate_completed() (mirrors the vector engine's prepare_needed gate).
  gboolean (*validate_completed)(const struct dt_remote_band_context_t *ctx,
                                 const void *new_params,
                                 dt_remote_error_t **error);
} dt_remote_band_module_adapter_t;

/** Per-call adapter context. All pointers are borrowed for the duration of
 * dt_remote_band_apply_patch(); callbacks must not retain them. */
typedef struct dt_remote_band_context_t
{
  const struct dt_iop_module_t *module;
  const dt_introspection_t *introspection;
  const dt_remote_band_module_adapter_t *adapter;
} dt_remote_band_context_t;

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

/** looks up the adapter registered for the stable IOP operation name
 * `operation` at `params_version`, or %NULL if no adapter covers that
 * (operation, params_version) pair -- including when `operation` is not a
 * band-bearing op at all. Native array fields on such ops remain
 * unsupported and no semantic band-set is advertised; this is not an
 * error. */
const dt_remote_band_module_adapter_t *
dt_remote_band_registry_lookup(const char *operation, guint params_version);

/* Test-only/internal seam for exercising semantic reads with synthetic
 * adapters through the public list_schema/read_values/apply_patch APIs,
 * mirroring dt_remote_vector_registry_set_lookup_override() (remote_vector.h)
 * exactly -- same rationale: the production registry table starts empty in
 * this task and is populated by later tasks, so the engine must be provable
 * against a hand-rolled adapter/real module .so pair before any adapter
 * ships. The override is process-wide and not thread-safe; tests must
 * install it only from a single-threaded process and reset it after each
 * test. Pass %NULL to restore the production registry lookup. Production
 * code never sets an override. */
typedef const dt_remote_band_module_adapter_t *(*dt_remote_band_registry_lookup_override_t)(
  const char *operation, guint params_version);
void dt_remote_band_registry_set_lookup_override(dt_remote_band_registry_lookup_override_t lookup);

/** validates that `adapter`'s nonempty operation matches `so`, and that the
 * nonnegative introspection params version is within its ordered inclusive
 * version range; that positive band/prepare-field counts have non-NULL
 * backing pointers and every prepare-field ID is nonempty; and, for every
 * descriptor, that its ID is nonempty, its `count` is nonzero, its
 * y_minimum/y_maximum are finite, ordered, and both lie within
 * `[-FLT_MAX, FLT_MAX]` (the native float range the write path narrows
 * into); that its x_policy is exhaustively checked: FIXED requires
 * `minimum_gap == 0.0`; INTERIOR requires a finite, strictly positive
 * `minimum_gap`; unknown x_policy values are rejected. It then validates
 * that `native_x` and `native_y` each resolve against `so`'s real
 * introspection tree to an array leaf whose declared and non-NULL element
 * descriptor are both `sizeof(float)` floats, whose length equals `count`
 * exactly (unlike a vector's native_capacity, a band has no preserved
 * tail), and whose own aggregate byte size equals `count * sizeof(float)`
 * exactly (overflow-checked); that no two descriptors on `adapter` share a
 * name; that no descriptor's name collides with a curve or vector semantic
 * ID registered for the same (operation, params_version) pair (queried
 * through dt_remote_curve_registry_lookup() and
 * dt_remote_vector_registry_lookup()); and, when `x_shared_with` is
 * non-NULL, that it names another descriptor on the same adapter (never
 * itself required, but never rejected either), that the two descriptors'
 * `count` match, and that the twin's own `x_shared_with` points back
 * (symmetry). Invalid metadata fails closed before its pointer is
 * traversed. The intrinsic adapter/descriptor validation result is cached
 * for the process lifetime, keyed by (`adapter` identity, introspection's
 * params_version) -- same caching convention as
 * dt_remote_vector_registry_validate(). Cross-class uniqueness is
 * evaluated on every call so it reflects the current curve/vector-registry
 * lookup state.
 *
 * Returns TRUE iff every check above passes. Returns FALSE, with `*error`
 * set to a newly allocated DT_REMOTE_ERR_INTERNAL (caller frees with
 * dt_remote_error_free()), otherwise -- this is always a registry/
 * introspection drift bug, never caller error. */
gboolean dt_remote_band_registry_validate(const dt_remote_band_module_adapter_t *adapter,
                                          const struct dt_iop_module_so_t *so,
                                          dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* band engine API                                                         */
/* ---------------------------------------------------------------------- */

/** Per (operation, params_version) schema listing: looks up the adapter for
 * `so`'s operation/params_version, validates its registry shape
 * (dt_remote_band_registry_validate() above, cached), then returns one
 * owned dt_remote_band_schema_t per descriptor, in registry order
 * (deep-copying every string -- the descriptor's own strings are static and
 * outlive nothing past this call). An operation/version with no registered
 * adapter succeeds with `*out_fields` set to %NULL (silent degrade, same
 * spirit as the vector registry's no-adapter path). Registry validation
 * failure instead fails closed: no partial descriptor list is returned,
 * `*out_fields` remains %NULL, and `*error` is DT_REMOTE_ERR_INTERNAL. On
 * success with at least one descriptor, sets `*out_fields` to a newly
 * allocated GPtrArray (owned by the caller, element destructor already set
 * to dt_remote_band_schema_free -- free with g_ptr_array_unref()). A
 * descriptor without writable_when is unconditionally
 * DT_REMOTE_WRITABLE_NOW; one with a predicate is
 * DT_REMOTE_WRITABLE_CONDITIONAL. Every returned schema's `x` field is
 * %NULL: unlike dt_remote_band_read_values() below, this entry point has
 * no live params blob to resolve current positions from (only `so`, never
 * a `dt_iop_module_t` instance). Null arguments and missing introspection
 * also fail with DT_REMOTE_ERR_INTERNAL and a %NULL output. */
gboolean dt_remote_band_list_schema(const struct dt_iop_module_so_t *so,
                                    GPtrArray **out_fields,   /* dt_remote_band_schema_t* */
                                    dt_remote_error_t **error);

/** Per live-instance value read: looks up + validates the adapter for
 * `module`'s operation/params_version (as above), then for every descriptor
 * evaluates active_when/writable_when against `params` (not necessarily
 * `module->params` -- callers may pass a projected/candidate block) and
 * reads the live y/x float components through dt_remote_path_resolve(),
 * widening each to a double. An operation/version with no registered
 * adapter succeeds with an empty table. Registry validation failure,
 * predicate/path resolution failure fails the whole read with
 * DT_REMOTE_ERR_INTERNAL: any temporary values are freed and `*out` remains
 * %NULL, never a partial result. On success, sets `*out` to a newly
 * allocated GHashTable (name -> owned dt_remote_band_value_t, owned by
 * the caller -- free with g_hash_table_unref(), value destructor already
 * set to dt_remote_band_value_free()). Null arguments and missing
 * introspection also fail with DT_REMOTE_ERR_INTERNAL and a %NULL
 * output. */
gboolean dt_remote_band_read_values(const struct dt_iop_module_t *module,
                                    const void *params,
                                    GHashTable **out,         /* name -> dt_remote_band_value_t */
                                    dt_remote_error_t **error);

/** Self-contained band-class transaction: resolves its own adapter for
 * `module`'s operation/params_version, validates and writes every entry in
 * `entries` in descriptor/registry order, then runs completed-state
 * validation -- all operating only on `new_params` (`old_params` remains
 * the pre-transaction block, read-only). Every element of `entries` must be
 * a DT_REMOTE_PARAMETER_BANDS-tagged dt_remote_semantic_patch_t; callers
 * guarantee this by construction and a violation is asserted, not skipped --
 * unlike dt_remote_band_apply_patch() below, this entry point never sees
 * another class's entries to filter out.
 *
 * For each descriptor with a matching entry, in registry order: (1) its
 * active_when/writable_when are evaluated against `new_params`; an inactive
 * or non-writable descriptor fails with DT_REMOTE_ERR_UNSUPPORTED_FIELD
 * ("condition_not_satisfied"); (2) an entry carrying `x` against a FIXED
 * descriptor fails with DT_REMOTE_ERR_UNSUPPORTED_FIELD
 * ("x_not_supported"); (3) dt_remote_band_validate() below is run in the
 * double domain against the descriptor's OWN native x as stored in
 * `old_params` -- never `new_params` -- for endpoint pinning (INTERIOR
 * only): reading from `new_params` would let an earlier descriptor's twin-
 * sync write (item 5 below) make a later descriptor's own endpoint check
 * vacuous, since it would then compare the submitted x against a value a
 * twin just mirrored into it rather than this descriptor's true
 * pre-transaction endpoint; (4) when the descriptor has a `x_shared_with`
 * twin and the entry carries `x`, and the twin also has an entry in this
 * same slice that carries `x`, every component of the two candidate x
 * arrays must compare exactly equal (double `==`) or the whole transaction
 * fails with DT_REMOTE_ERR_INVALID_VALUE ("twin_conflict") -- detected
 * before any native write for either descriptor; (5) only once every check
 * above passes are floats narrowed and written: `y` always, to `native_y`;
 * `x`, when present, to `native_x` AND (when `x_shared_with` is set) to the
 * twin descriptor's `native_x` too, with the identical narrowed values --
 * this is the "an x drag mirrors into the twin" behavior the twin
 * relationship models, applied regardless of whether the twin itself has
 * its own entry in this call, and unconditionally: the twin's own endpoint
 * is never independently checked against the mirrored value, matching
 * atrous.c's own unconditional mirror-on-drag behavior the design doc
 * documents. Every other byte of `new_params` is left unchanged. `entries`
 * may be empty: an op with no registered band adapter
 * then trivially succeeds; an op with a registered adapter still runs
 * completed-state validation against `new_params` even with zero entries
 * (the same "validate on adapter presence alone" behavior the vector engine
 * has). Called only by dt_remote_band_apply_patch() below, which partitions
 * its own class's entries out of a full patch and calls this -- see that
 * wrapper's own doc comment for why remote_edit.c's class-ops dispatch
 * table calls the wrapper, never this function, directly. */
gboolean dt_remote_band_apply_entries(const struct dt_iop_module_t *module,
                                      const void *old_params,
                                      void *new_params,
                                      GPtrArray *entries, /* dt_remote_semantic_patch_t*, bands class only */
                                      dt_remote_error_t **error);

/** Applies the band-class portion of `patch` to the caller-owned projected
 * params block. Scalar entries must already have been written to
 * `new_params`; `old_params` remains the pre-transaction block. Every entry
 * in `patch->semantic_values` whose class_id is not DT_REMOTE_PARAMETER_BANDS
 * is skipped -- this engine only ever touches its own class, the same
 * request may carry curve/vector entries the curve/vector engines handle
 * separately. Thin wrapper over dt_remote_band_apply_entries() above:
 * partitions this class's entries out of `patch` (preserving request
 * order), then delegates -- mirrors dt_remote_vector_apply_patch() exactly,
 * including its prepare_needed gate: null checks; partition; a scalar-only
 * patch that names one of the adapter's `prepare_fields` still triggers
 * the registry-ordered pass (and therefore validate_completed()) even with
 * zero band entries; a scalar-only patch that neither carries band
 * semantics nor mentions a prepare field returns TRUE immediately without
 * ever looking up or validating the (possibly empty/invalid) registry.
 * This is the entry point remote_edit.c's class-ops dispatch table calls
 * (unconditionally, with the whole patch, never a pre-partitioned slice). */
gboolean dt_remote_band_apply_patch(const struct dt_iop_module_t *module,
                                    const void *old_params,
                                    void *new_params,
                                    const dt_remote_patch_t *patch,
                                    dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* common band validator                                                   */
/* ---------------------------------------------------------------------- */

/** Validates a candidate band patch (`y`, required; `x`, nullable) against
 * `desc`:
 *
 *   - `y->len` must equal `desc->count` ("count_mismatch");
 *   - every y component must be finite ("non_finite") -- defense in depth,
 *     the parser already guarantees this on entries it hands the engine;
 *   - each finite y component must fall within `[desc->y_minimum,
 *     desc->y_maximum]`, inclusive ("domain") -- compared entirely in
 *     double precision, so a value like 1.00000001 against a maximum of
 *     1.0 is correctly rejected even though both would round to the same
 *     float;
 *   - each finite, in-domain y component must also lie within
 *     `[-FLT_MAX, FLT_MAX]` ("native_range") -- a defensive check
 *     independent of `desc->y_minimum`/`y_maximum`, since this function is
 *     pure and directly callable without going through
 *     dt_remote_band_registry_validate()'s matching bound cap;
 *   - when `x` is non-%NULL and `desc->x_policy` is DT_REMOTE_BAND_X_INTERIOR:
 *     `x->len` must equal `desc->count` ("count_mismatch"); the narrowed
 *     endpoints must match the currently stored ones -- `(float)x[0] !=
 *     stored_x[0]` or `(float)x[count-1] != stored_x[count-1]`
 *     ("endpoint"); adjacent pairs must be strictly ascending ("unordered"
 *     when the delta is <= 0.0) with a gap of at least `desc->minimum_gap`
 *     ("gap" when the delta is strictly less -- a delta exactly equal to
 *     minimum_gap is accepted). `stored_x` must have `desc->count` valid
 *     floats whenever `x` is non-%NULL under INTERIOR;
 *   - when `x` is non-%NULL and `desc->x_policy` is DT_REMOTE_BAND_X_FIXED,
 *     `x` is not checked here at all -- a FIXED descriptor never accepts a
 *     write-time `x` in the first place (dt_remote_band_apply_entries()
 *     rejects it upfront with DT_REMOTE_ERR_UNSUPPORTED_FIELD, a field-level
 *     concern this pure value-level validator does not model, mirroring how
 *     dt_remote_vector_apply_patch() checks active_when/writable_when
 *     itself rather than folding them into dt_remote_vector_validate()).
 *
 * Every rejection returns FALSE with `*error` set to a newly allocated
 * DT_REMOTE_ERR_INVALID_VALUE (caller frees with dt_remote_error_free());
 * `error->details_json` is a serialized JSON object with a "parameter"
 * member (`desc->name`), a "constraint" member (one of the short stable
 * strings named above), an "array" member ("y" or "x") naming which array
 * the violation is attributable to, and an "index" member (0-based)
 * whenever the violation is component-specific -- omitted for the
 * whole-array "count_mismatch" failures. `y` and `x` are never modified, on
 * any path, including rejection.
 *
 * Pure function: no introspection calls, no registry lookups, no JSON
 * parsing of the input. */
gboolean dt_remote_band_validate(const dt_remote_band_descriptor_t *desc,
                                 const GArray *y, /* double */
                                 const GArray *x, /* double, nullable */
                                 const float *stored_x, /* desc->count floats, endpoint pinning */
                                 dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
