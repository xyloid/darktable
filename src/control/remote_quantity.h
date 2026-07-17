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
// semantic-quantity machinery: the registry descriptor/adapter types and
// the registry/validator/apply API that build on them. See the milestone-6
// quantity-class design doc (SS Class model, SS Conversion authority,
// SS Coefficient coexistence) for the normative definitions this header
// follows.
//
// Unlike every prior class (curve/vector/bands), a quantity descriptor has
// NO native introspection path of its own: the presentation-domain values
// (e.g. Kelvin/tint) are never stored directly in the params blob, only
// derived from it through a module-owned conversion. That conversion is
// exposed as an optional iop API function pair
// (`remote_quantity_read`/`remote_quantity_write`, src/iop/iop_api.h,
// resolved on `dt_iop_module_so_t` at plugin load) which this engine calls
// instead of walking introspection offsets itself. `native_fields` on the
// module adapter names the SCALAR params fields that write hook is allowed
// to touch (e.g. temperature's `red`/`green`/`blue`/`various`); those
// fields stay independently writable as ordinary scalars (the "coefficient
// coexistence" deliberate exception -- see the design doc), so the engine
// must detect and reject a request that writes one of them AND a quantity
// entry for the same adapter in the same patch (the "native conflict"
// rule, generalizing the band/vector twin-conflict idea to scalar-vs-
// semantic overlap), and must byte-verify after every hook write that no
// byte of the params blob outside those declared fields moved.
//
// Neutral (non-introspection) quantity schema/value/patch types live in
// control/remote_parameters.h, not here -- see that header's comment for
// the file split rationale (same reasoning as remote_band.h's own
// comment).
//
// Threading: like remote_band.h/remote_vector.h/remote_curve.h/
// remote_edit.h, any code that resolves a path against a live darkroom
// module's params block, or that calls a module's remote_quantity_read/
// write hook (which may read live GUI state), must do so from the GTK
// main thread; the descriptor/adapter types themselves are inert
// compiled-in data with no such constraint.

#pragma once

#include "common/introspection.h"
#include "control/remote_curve.h" // dt_remote_introspection_path_t, dt_remote_path_resolve(),
                                  // dt_remote_parameter_predicate_t
#include "control/remote_edit.h" // dt_remote_error_t, dt_remote_patch_t
#include "control/remote_parameters.h" // dt_remote_quantity_component_value_t, dt_remote_quantity_patch_t

#include <glib.h>

G_BEGIN_DECLS

// Wire-layer cap on how many named components a single quantity's `values`
// object may carry (src/control/remote_protocol.c's own definition of the
// same wire-layer constant, duplicated here so the registry can enforce it
// on `component_count` without a cross-layer include -- the two must stay
// numerically identical; remote_protocol.c is not included from this
// engine layer, so there is no macro-redefinition conflict).
#define DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP 8

/* ---------------------------------------------------------------------- */
/* registry descriptor types                                              */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_quantity_component_descriptor_t
{
  const char *name;
  const char *unit;        // NULL = unitless
  double minimum, maximum;
} dt_remote_quantity_component_descriptor_t;

typedef struct dt_remote_quantity_descriptor_t
{
  const char *name;               // stable semantic ID, e.g. "wb.temperature"
  const char *display_name;
  const char *description;
  const dt_remote_quantity_component_descriptor_t *components;
  guint component_count;
  gboolean derived;               // TRUE for every v1 adapter
  const dt_remote_parameter_predicate_t *active_when;
  const dt_remote_parameter_predicate_t *writable_when;
} dt_remote_quantity_descriptor_t;

/* ---------------------------------------------------------------------- */
/* module adapter                                                          */
/* ---------------------------------------------------------------------- */

struct dt_iop_module_so_t; // develop/imageop.h; kept a bare forward reference,
                           // same rationale as remote_band.h's own.
struct dt_iop_module_t;

typedef struct dt_remote_quantity_module_adapter_t
{
  const char *operation;
  guint minimum_params_version, maximum_params_version;
  const dt_remote_quantity_descriptor_t *quantities;
  guint quantity_count;
  const char *const *native_fields;   // scalar params fields the write hook
  guint native_field_count;           // may touch; also the conflict set
} dt_remote_quantity_module_adapter_t;

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

/** looks up the adapter registered for the stable IOP operation name
 * `operation` at `params_version`, or %NULL if no adapter covers that
 * (operation, params_version) pair -- including when `operation` has no
 * quantity semantics at all. This is not an error: an op with no quantity
 * adapter simply advertises no derived-quantity fields. */
const dt_remote_quantity_module_adapter_t *
dt_remote_quantity_registry_lookup(const char *operation, guint params_version);

/* Test-only/internal seam for exercising the engine with synthetic
 * adapters through the public list_schema/read_values/apply_patch APIs,
 * mirroring dt_remote_band_registry_set_lookup_override() (remote_band.h)
 * exactly -- same rationale: the production registry table starts empty in
 * this task and is populated by a later task, so the engine must be
 * provable against a hand-rolled adapter/real module .so pair before any
 * adapter ships. The override is process-wide and not thread-safe; tests
 * must install it only from a single-threaded process and reset it after
 * each test. Pass %NULL to restore the production registry lookup.
 * Production code never sets an override. */
typedef const dt_remote_quantity_module_adapter_t *(*dt_remote_quantity_registry_lookup_override_t)(
  const char *operation, guint params_version);
void dt_remote_quantity_registry_set_lookup_override(dt_remote_quantity_registry_lookup_override_t lookup);

/** validates that `adapter`'s nonempty operation matches `so`, and that the
 * nonnegative introspection params version is within its ordered inclusive
 * version range; that a positive quantity/native-field count has a
 * non-NULL backing pointer and every native-field name is nonempty; and,
 * for every descriptor, that its ID is nonempty, its `component_count` is
 * neither zero nor greater than DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP, and
 * every component has a nonempty name and finite, ordered
 * (`minimum < maximum`) bounds. It then validates that `so` exports BOTH
 * `remote_quantity_read` and `remote_quantity_write` (fails closed
 * otherwise) UNLESS a hooks override is currently installed (see
 * dt_remote_quantity_set_hooks_override() below), in which case the
 * so-level hook presence is not required -- production code never installs
 * an override, so this exception only ever applies to tests exercising the
 * engine against an op that has no real conversion hooks; that no two
 * descriptors on `adapter` share a name, and no two components of the same
 * descriptor share a name; that no descriptor's name collides with a curve,
 * vector, or band semantic ID registered for the same (operation,
 * params_version) pair (queried through dt_remote_curve_registry_lookup(),
 * dt_remote_vector_registry_lookup(), and dt_remote_band_registry_lookup());
 * and that every name in `adapter->native_fields` resolves against `so`'s
 * real introspection tree to a writable (not on the per-op denylist,
 * dt_remote_denylist_for_op()) scalar `float` leaf. Invalid metadata fails
 * closed before its pointer is traversed. The intrinsic adapter/descriptor
 * validation result is cached for the process lifetime, keyed by (`adapter`
 * identity, introspection's params_version) -- same caching convention as
 * dt_remote_band_registry_validate(). Cross-class uniqueness and hook
 * presence are evaluated on every call so they reflect the current
 * curve/vector/band-registry and hooks-override state.
 *
 * Returns TRUE iff every check above passes. Returns FALSE, with `*error`
 * set to a newly allocated DT_REMOTE_ERR_INTERNAL (caller frees with
 * dt_remote_error_free()), otherwise -- this is always a registry/
 * introspection drift bug, never caller error. */
gboolean dt_remote_quantity_registry_validate(const dt_remote_quantity_module_adapter_t *adapter,
                                              const struct dt_iop_module_so_t *so,
                                              dt_remote_error_t **error);

/* Test-only/internal seam replacing the module-hook conversion with a
 * caller-supplied pair, so the engine can be proven end-to-end (registry
 * validate's hook-presence check, dt_remote_quantity_read_values(), and
 * dt_remote_quantity_apply_entries()) without depending on a module's real
 * conversion, which may itself depend on live GUI state the unit-test
 * harness never builds (temperature.c's hooks fail closed without
 * `gui_data`, by design). Both members are process-wide, not thread-safe,
 * and must only be installed/reset from a single-threaded test process
 * (same discipline as the lookup override above). Pass NULL/NULL to
 * restore the production behavior (call the module's own so-resolved
 * hooks). Production code never installs an override. */
typedef gboolean (*dt_remote_quantity_read_hook_t)(struct dt_iop_module_t *,
                                                   const void *, double *, size_t);
typedef gboolean (*dt_remote_quantity_write_hook_t)(struct dt_iop_module_t *,
                                                    const double *, size_t, void *);
void dt_remote_quantity_set_hooks_override(dt_remote_quantity_read_hook_t read,
                                           dt_remote_quantity_write_hook_t write);

/* ---------------------------------------------------------------------- */
/* quantity engine API                                                     */
/* ---------------------------------------------------------------------- */

/** Per (operation, params_version) schema listing: looks up the adapter for
 * `so`'s operation/params_version, validates its registry shape
 * (dt_remote_quantity_registry_validate() above, cached), then returns one
 * owned dt_remote_quantity_schema_t per descriptor, in registry order
 * (deep-copying every string and component -- the descriptor's own strings
 * are static and outlive nothing past this call). No hook is called and no
 * params block is consulted: this is static registry data only. An
 * operation/version with no registered adapter succeeds with `*out_fields`
 * set to %NULL (silent degrade, same spirit as the band/vector registries'
 * no-adapter path). Registry validation failure instead fails closed: no
 * partial descriptor list is returned, `*out_fields` remains %NULL, and
 * `*error` is DT_REMOTE_ERR_INTERNAL. On success with at least one
 * descriptor, sets `*out_fields` to a newly allocated GPtrArray (owned by
 * the caller, element destructor already set to
 * dt_remote_quantity_schema_free -- free with g_ptr_array_unref()). A
 * descriptor without writable_when is unconditionally
 * DT_REMOTE_WRITABLE_NOW; one with a predicate is
 * DT_REMOTE_WRITABLE_CONDITIONAL. Null arguments and missing introspection
 * also fail with DT_REMOTE_ERR_INTERNAL and a %NULL output. */
gboolean dt_remote_quantity_list_schema(const struct dt_iop_module_so_t *so,
                                        GPtrArray **out_fields,  /* dt_remote_quantity_schema_t* */
                                        dt_remote_error_t **error);

/** Per live-instance value read: looks up + validates the adapter for
 * `module`'s operation/params_version (as above), then for every descriptor
 * evaluates active_when/writable_when against `params` (not necessarily
 * `module->params` -- callers may pass a projected/candidate block, same
 * convention as dt_remote_band_read_values()) and calls the conversion read
 * hook (the installed override when set, otherwise
 * `module->so->remote_quantity_read`) with a `component_count`-sized double
 * buffer. An operation/version with no registered adapter succeeds with an
 * empty table. Registry validation failure, a missing hook (should never
 * happen once registry_validate has passed, but checked defensively), or
 * the hook itself returning FALSE fails the whole read with
 * DT_REMOTE_ERR_INTERNAL -- never a crash: any temporary values are freed
 * and `*out` remains %NULL, never a partial result. On success, sets `*out`
 * to a newly allocated GHashTable (name -> owned dt_remote_quantity_value_t,
 * owned by the caller -- free with g_hash_table_unref(), value destructor
 * already set to dt_remote_quantity_value_free()). Null arguments and
 * missing introspection also fail with DT_REMOTE_ERR_INTERNAL and a %NULL
 * output. */
gboolean dt_remote_quantity_read_values(const struct dt_iop_module_t *module,
                                        const void *params,
                                        GHashTable **out,        /* name -> dt_remote_quantity_value_t */
                                        dt_remote_error_t **error);

/** Self-contained quantity-class transaction: resolves its own adapter for
 * `module`'s operation/params_version, validates and writes every entry in
 * `entries` in descriptor/registry order, all operating only on
 * `new_params` (`old_params` remains the pre-transaction block, read-only,
 * currently unused by this engine -- kept for signature symmetry with the
 * curve/vector/band twins and in case a future revision needs it).
 *
 * Every element of `entries` must be a DT_REMOTE_PARAMETER_QUANTITY-tagged
 * dt_remote_semantic_patch_t; callers guarantee this by construction and a
 * violation is asserted, not skipped.
 *
 * `patch` carries the FULL request (including scalar_values) purely so this
 * function can enforce the native-conflict rule below; it is borrowed, may
 * be %NULL (pure-engine tests that only care about quantity-vs-quantity
 * behavior), and is never itself applied here (scalar entries are the
 * scalar engine's job, already run before this function is reached in the
 * production dispatch order).
 *
 * For each entry, in registry order: (1) its ID must resolve to a
 * descriptor on `adapter` -- an unresolved ID fails the WHOLE call with
 * DT_REMOTE_ERR_UNKNOWN_FIELD, `new_params` untouched; a name repeated
 * across `entries` fails with DT_REMOTE_ERR_INVALID_VALUE
 * ("duplicate_parameter"); (2) dt_remote_quantity_validate() below is run
 * in the double domain against every entry BEFORE any of them writes
 * anything, so a later entry's rejection never leaves an earlier entry's
 * write in `new_params`; (3) once every entry is known valid, the
 * NATIVE-CONFLICT rule is checked: when `entries` is nonempty (at least one
 * quantity entry targets this adapter) and `patch` is non-%NULL and
 * `patch->scalar_values` contains any entry whose `name` matches one of
 * `adapter->native_fields`, the WHOLE call fails with
 * DT_REMOTE_ERR_INVALID_VALUE ("native_conflict"), `new_params` untouched --
 * this is the "coefficient coexistence" exception's enforcement point (see
 * this header's own top comment); (4) only once every check above passes,
 * for each descriptor with a matching entry: its active_when/writable_when
 * are evaluated against `new_params` (band-engine ordering: earlier
 * entries' writes in this same call are visible to a later entry's
 * predicate) -- an inactive or non-writable descriptor fails with
 * DT_REMOTE_ERR_UNSUPPORTED_FIELD ("condition_not_satisfied"); (5) the
 * conversion write hook (the installed override when set, otherwise
 * `module->so->remote_quantity_write`) is called against `new_params` with
 * the entry's values in descriptor component order; a hook returning FALSE
 * fails with DT_REMOTE_ERR_INTERNAL, `new_params` restored to its
 * pre-write-attempt state (never left partially written by a failed hook
 * call); (6) after every successful hook call, a BYTE-CHECK verifies that
 * every byte of `new_params` outside the byte ranges `native_fields`
 * resolves to is identical to its value immediately before that hook call
 * -- a hook that mutates any other byte (a conversion bug, never a caller
 * error) fails the whole call with DT_REMOTE_ERR_INTERNAL and `new_params`
 * is restored to its pre-hook-call state (the caller's own temp-blob
 * discipline then discards the whole scratch block, per the mutation
 * engine's normal rollback-on-rejection contract).
 *
 * `entries` may be empty: an op with no registered quantity adapter then
 * trivially succeeds (see dt_remote_quantity_apply_patch() below, which is
 * the only production caller and which never reaches this function unless
 * `entries` is nonempty); an op WITH a registered adapter called directly
 * with zero entries also trivially succeeds after registry validation --
 * unlike the band/vector engines, there is no `validate_completed`
 * callback and no `prepare_fields` gate on a quantity adapter, so there is
 * nothing left to check with zero entries. */
gboolean dt_remote_quantity_apply_entries(const struct dt_iop_module_t *module,
                                          const void *old_params, void *new_params,
                                          GPtrArray *entries, /* dt_remote_semantic_patch_t*, quantity class only */
                                          const dt_remote_patch_t *patch, /* for the
                                          scalar-conflict check; may be NULL in
                                          pure-engine tests */
                                          dt_remote_error_t **error);

/** Applies the quantity-class portion of `patch` to the caller-owned
 * projected params block. `old_params` remains the pre-transaction block
 * (read-only, currently unused -- see dt_remote_quantity_apply_entries()'s
 * own note). Every entry in `patch->semantic_values` whose class_id is not
 * DT_REMOTE_PARAMETER_QUANTITY is skipped -- this engine only ever touches
 * its own class, the same request may carry curve/vector/band entries the
 * other engines handle separately. Thin wrapper over
 * dt_remote_quantity_apply_entries() above: partitions this class's
 * entries out of `patch` (preserving request order), then delegates,
 * passing `patch` itself through unchanged for the native-conflict check --
 * mirrors dt_remote_band_apply_patch() exactly, EXCEPT there is no
 * prepare_fields gate to evaluate first (a quantity adapter has none): a
 * patch carrying zero quantity entries returns TRUE immediately without
 * ever looking up or validating the (possibly empty/invalid) registry, and
 * one carrying at least one quantity entry always runs the full lookup +
 * validate + apply path. This is the entry point remote_edit.c's class-ops
 * dispatch table calls (unconditionally, with the whole patch, never a
 * pre-partitioned slice). */
gboolean dt_remote_quantity_apply_patch(const struct dt_iop_module_t *module,
                                        const void *old_params, void *new_params,
                                        const dt_remote_patch_t *patch,
                                        dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* common quantity validator                                               */
/* ---------------------------------------------------------------------- */

/** Validates a candidate quantity patch's named component values against
 * `desc`:
 *
 *   - the NAME SET of `values` must equal `desc`'s component name set
 *     EXACTLY: a name in `values` not present on `desc` is
 *     ("unknown_component"); a name repeated within `values` is
 *     ("duplicate_component"); a `desc` component absent from `values` is
 *     ("missing_component");
 *   - once the name sets are known to match exactly, each component's
 *     value, in descriptor order, must be finite ("non_finite") and must
 *     fall within its own descriptor's `[minimum, maximum]`, inclusive
 *     ("domain") -- compared entirely in double precision, so a value like
 *     25000.00001 against a maximum of 25000.0 is correctly rejected.
 *
 * Every rejection returns FALSE with `*error` set to a newly allocated
 * DT_REMOTE_ERR_INVALID_VALUE (caller frees with dt_remote_error_free());
 * `error->details_json` is a serialized JSON object with a "parameter"
 * member (`desc->name`), a "component" member (the offending component's
 * name -- every rejection above is attributable to exactly one component),
 * and a "constraint" member (one of the short stable strings named above).
 * `values` is never modified, on any path, including rejection.
 *
 * Pure function: no introspection calls, no registry lookups, no hook
 * calls, no JSON parsing of the input. */
gboolean dt_remote_quantity_validate(const dt_remote_quantity_descriptor_t *desc,
                                     const GPtrArray *values, /* dt_remote_quantity_component_value_t* */
                                     dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
