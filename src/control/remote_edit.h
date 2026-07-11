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

// Internal boundary for the darktable MCP remote-edit sidecar: neutral
// data types plus synchronous, read-only functions that walk live
// darkroom modules and convert module introspection into neutral
// schemas/values. No sockets, no JSON, no mutation here -- see the
// remote-edit internals design doc for the full protocol built on top
// of this boundary.
//
// Threading: every function here must be called from the GTK main
// thread (the same thread that owns darktable.develop and module
// params); see the internals doc §1.

#pragma once

#include "common/introspection.h"

#include <gio/gio.h>   // GCancellable (dt_remote_render_preview_execute)
#include <glib.h>
#include <inttypes.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* errors                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_error_code_t
{
  DT_REMOTE_OK = 0,
  DT_REMOTE_ERR_NOT_IN_DARKROOM,
  DT_REMOTE_ERR_NO_IMAGE_OPEN,
  DT_REMOTE_ERR_UNKNOWN_MODULE,
  DT_REMOTE_ERR_UNKNOWN_INSTANCE,
  DT_REMOTE_ERR_UNKNOWN_FIELD,
  DT_REMOTE_ERR_UNSUPPORTED_FIELD,
  DT_REMOTE_ERR_INVALID_VALUE,
  DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED,
  DT_REMOTE_ERR_REVISION_CONFLICT,
  DT_REMOTE_ERR_PREVIEW_FAILED,
  DT_REMOTE_ERR_SCOPE_FAILED,
  DT_REMOTE_ERR_INTERNAL,
} dt_remote_error_code_t;
// transport-only codes (unauthorized, request_too_large, busy) live in
// remote_server/remote_protocol, not here -- remote_edit never sees them.

typedef struct dt_remote_error_t
{
  dt_remote_error_code_t code;
  char *message;        // owned; human-readable
  char *details_json;   // owned, nullable; pre-serialized details object
} dt_remote_error_t;

/** frees an error allocated by any dt_remote_* function. NULL-safe. */
void dt_remote_error_free(dt_remote_error_t *error);

/* ---------------------------------------------------------------------- */
/* neutral value types                                                     */
/* ---------------------------------------------------------------------- */

typedef enum dt_remote_value_type_t
{
  DT_REMOTE_VALUE_FLOAT,   // float and double fields
  DT_REMOTE_VALUE_INT,     // all signed/unsigned integer widths
  DT_REMOTE_VALUE_BOOL,
  DT_REMOTE_VALUE_ENUM,
} dt_remote_value_type_t;

typedef struct dt_remote_value_t
{
  dt_remote_value_type_t type;
  union
  {
    double f;
    int64_t i;
    gboolean b;
    struct { int value; char *name; } e;  // name owned
  } v;
} dt_remote_value_t;

/** frees any memory owned by a value in place (e.g. the enum name);
 * does not free `v` itself. NULL-safe. */
void dt_remote_value_clear(dt_remote_value_t *v);

typedef struct dt_remote_enum_value_t
{
  char *name;          // stable identifier; points into static introspection
                       // data (module .so lifetime), not owned/freed here
  int value;
  char *description;   // $DESCRIPTION or ""; same lifetime note as name
} dt_remote_enum_value_t;

typedef struct dt_remote_field_t
{
  char *name;           // introspection name, dotted for nested leaves;
                       // points into static introspection data, not owned
  char *description;    // $DESCRIPTION or ""; not owned (see name)
  char *type_name;      // "float", "int", "uint", "bool", "enum",
                        // "array", "string", "struct", "opaque"; static
                        // literal, not owned
  gboolean writable;
  gboolean has_range;
  double minimum, maximum;   // valid iff has_range
  gboolean has_default;
  dt_remote_value_t default_value;
  GPtrArray *enum_values;    // of dt_remote_enum_value_t {name,value,desc};
                            // owned, NULL unless type_name == "enum"
} dt_remote_field_t;

/** frees a single field, including its owned default_value and
 * enum_values. NULL-safe. Suitable as a GDestroyNotify. */
void dt_remote_field_free(gpointer field);

/* ---------------------------------------------------------------------- */
/* module addressing                                                       */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_module_ref_t
{
  const char *op;            // not owned; borrowed from the caller
  int instance;              // multi_priority; 0 = default
} dt_remote_module_ref_t;

// Per-module list of dotted introspection names forced to `writable:false`
// regardless of type -- bookkeeping/auto-derived fields that pass the type
// filter but must not be offered as editable (see the supported-operations
// design for the concrete per-op tables; populating those tables is out of
// scope for this step). NULL, or a denylist with names == NULL, denies
// nothing.
typedef struct dt_remote_denylist_t
{
  const char *const *names;  // NULL-terminated array of dotted field names;
                             // not owned
} dt_remote_denylist_t;

/** TRUE iff `name` (a dotted introspection name) appears in `denylist`.
 * NULL-safe: a NULL denylist, a denylist with names == NULL, or a NULL
 * name all return FALSE (deny nothing). */
gboolean dt_remote_denylisted(const dt_remote_denylist_t *denylist, const char *name);

/** static per-op forced-writable:false table (supported-operations appendix).
 * NULL for ops with no entry -- meaning deny nothing. */
const dt_remote_denylist_t *dt_remote_denylist_for_op(const char *op);

/* ---------------------------------------------------------------------- */
/* result types -- mirror the wire shapes in the protocol reference        */
/* field-for-field; the dispatcher serializes them 1:1.                    */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_state_t   // <- get_state
{
  char *view;                   // owned; stable, untranslated identifier
                               // for the current view (e.g. "darkroom",
                               // "lighttable", "none"); never a display
                               // string
  gboolean has_image;           // FALSE -> image fields undefined, wire null
  int32_t image_id;
  char *image_filename;         // owned; basename only
  int width, height;
  char *maker, *model, *lens;   // owned; "" when unknown
  float iso, aperture, exposure_time, focal_length;  // 0 when unknown
  uint64_t revision;
} dt_remote_state_t;

/** frees a state snapshot returned by dt_remote_get_state. NULL-safe. */
void dt_remote_state_free(dt_remote_state_t *state);

typedef struct dt_remote_module_t  // <- list_modules entries
{
  char *op;                     // owned; stable identifier
  int instance;                 // multi_priority
  char *instance_name;          // owned; translated, "" for default
  char *display_name;           // owned; translated, presentation only
  gboolean enabled;
  gboolean deprecated;
  gboolean supports_multiple_instances;
} dt_remote_module_t;

/** frees a single module list entry. Suitable as a GDestroyNotify. */
void dt_remote_module_free(gpointer module);

typedef struct dt_remote_module_schema_t  // <- get_module_schema
{
  char *op;                     // owned
  char *display_name;           // owned
  int params_version;           // module class version()
  gboolean deprecated;
  gboolean supports_multiple_instances;
  GPtrArray *fields;            // owned; dt_remote_field_t
  GPtrArray *semantic_fields;   // owned; dt_remote_curve_schema_t
                                // (remote_parameters.h), NULL when the op
                                // has no registered semantic curves
} dt_remote_module_schema_t;

/** frees a module schema, including primitive and semantic fields. NULL-safe. */
void dt_remote_module_schema_free(dt_remote_module_schema_t *schema);

typedef struct dt_remote_history_item_t  // <- get_history items
{
  int seq;                      // history stack position
  char *op;                     // owned
  int instance;
  char *display_name;           // owned
  char *instance_name;          // owned
  gboolean enabled;
} dt_remote_history_item_t;

/** frees a single history item. Suitable as a GDestroyNotify. */
void dt_remote_history_item_free(gpointer item);

typedef struct dt_remote_patch_entry_t
{
  char *name;                 // owned
  dt_remote_value_t value;
} dt_remote_patch_entry_t;

/** frees a single patch entry (name and any owned value memory).
 * Suitable as a GDestroyNotify. */
void dt_remote_patch_entry_free(gpointer entry);

typedef struct dt_remote_patch_t
{
  GPtrArray *scalar_values;    // dt_remote_patch_entry_t
  GPtrArray *semantic_values;  // dt_remote_semantic_patch_t (remote_parameters.h);
                               // nullable. The neutral types exist as of this
                               // struct, but nothing constructs or reads this
                               // array yet -- the introspection cursor,
                               // validator, and registry that populate and
                               // apply it land in later steps (see the
                               // curve-classes design)
  gboolean has_enable;
  gboolean enable;
} dt_remote_patch_t;
// duplicates rejected at protocol layer

// Shared by every mutation (set/enable/reset/create): all members are
// read back from live state after commit, inside the same main-context
// dispatch -- the caller never echoes what it sent. Defined here as part
// of the normative type boundary; the mutation functions that produce it
// land in a later step.
typedef struct dt_remote_mutation_result_t
{
  char *op;                     // owned
  int instance;                 // the new instance for create_module_instance
  char *instance_name;          // owned; meaningful after create
  gboolean enabled;
  GPtrArray *values;            // dt_remote_patch_entry_t, read back; NULL
                                // for enable/create (no values on the wire)
  uint64_t revision;
} dt_remote_mutation_result_t;

/** frees a mutation result. NULL-safe. */
void dt_remote_mutation_result_free(dt_remote_mutation_result_t *result);

/* ---------------------------------------------------------------------- */
/* pure conversion layer (2a) -- testable with fixture                     */
/* dt_introspection_field_t values and no dt_develop_t.                    */
/* ---------------------------------------------------------------------- */

/** walks a get_introspection_linear()-style array (NONE-terminated) and
 * converts every entry -- leaves and non-leaf struct/union/array/opaque
 * summaries alike -- into a dt_remote_field_t. Unsupported types are
 * marked writable:false, never omitted. `denylist` may be NULL. Returns
 * a newly allocated, never-NULL GPtrArray of dt_remote_field_t owned by
 * the caller (element destructor already set; free with
 * g_ptr_array_unref()). */
GPtrArray *dt_remote_schema_from_introspection(const dt_introspection_field_t *linear,
                                               const dt_remote_denylist_t *denylist);

/** reads the current value of a single supported scalar field out of
 * params_blob (the start of the whole params struct that `f`'s offset is
 * relative to) into `out`. Returns FALSE without touching `out` if `f`'s
 * type is not one of the four v1 value classes. */
gboolean dt_remote_value_from_field(const dt_introspection_field_t *f,
                                    const void *params_blob,
                                    dt_remote_value_t *out);

/** validates `v` against `f` (type class, range/enum membership) and, if
 * valid, writes it into params_blob at f's offset. On failure returns
 * FALSE and allocates *err (DT_REMOTE_ERR_UNSUPPORTED_FIELD for a field
 * whose type is not one of the four v1 value classes, otherwise
 * DT_REMOTE_ERR_INVALID_VALUE); params_blob is left untouched. */
gboolean dt_remote_value_validate_and_write(const dt_introspection_field_t *f,
                                            const dt_remote_value_t *v,
                                            void *params_blob,
                                            dt_remote_error_t **err);

/** The pure core of dt_remote_set_module_params (plan step 7, internals §3
 * steps 4-6): resolves, validates, and writes every entry of
 * `patch->scalar_values` into `params_blob`, which must already be a
 * scratch copy of the module's params the caller allocated and populated
 * (e.g. via g_malloc(module->params_size) + memcpy from module->params) --
 * this function never sees or touches live module state, only the copy it
 * is handed, so a caller that discards `params_blob` on failure has by
 * construction changed nothing observable.
 *
 * `linear` is a get_introspection_linear()-style array (NONE-terminated);
 * `denylist` is the same per-module forced-writable:false table
 * dt_remote_schema_from_introspection() takes (NULL denies nothing).
 *
 * Processes entries in order, writing each one immediately via
 * dt_remote_value_validate_and_write() before moving to the next (not a
 * separate validate-then-write pass) -- this matches the normative
 * sequence exactly; it is safe because the whole block is scratch, per
 * above. Returns FALSE on the first rejected entry and leaves later
 * entries unprocessed:
 *
 *  - patch is NULL, or scalar_values, semantic_values, and has_enable are
 *    all empty/unset -> DT_REMOTE_ERR_INVALID_VALUE ("values must be
 *    non-empty", the wire contract's own requirement, enforced here too
 *    as a second line of defense); scalar_values alone may be NULL/empty
 *    when semantic_values is non-empty or has_enable is set;
 *  - an entry's name does not match any field in `linear` ->
 *    DT_REMOTE_ERR_UNKNOWN_FIELD;
 *  - an entry's name repeats an earlier entry in the same patch ->
 *    DT_REMOTE_ERR_INVALID_VALUE ("duplicate field"); the protocol layer
 *    cannot itself produce this (JSON object keys are already unique by
 *    the time they reach here), but a future non-JSON caller could, so
 *    this pure core still guards against it directly;
 *  - the field is denylisted -> DT_REMOTE_ERR_UNSUPPORTED_FIELD;
 *  - dt_remote_value_validate_and_write() rejects the value (wrong type,
 *    out of range/non-finite, unrecognized enum member, or a known
 *    non-scalar field type) -> whatever it sets (DT_REMOTE_ERR_INVALID_VALUE
 *    or DT_REMOTE_ERR_UNSUPPORTED_FIELD).
 *
 * `patch->semantic_values` (see remote_parameters.h) is not read or
 * applied here yet -- only its presence is consulted, for the emptiness
 * check above. "step 6, validate the completed parameter block" is
 * currently a no-op for scalar_values (every field is already
 * individually checked above, with no cross-field constraints in the v1
 * scalar schema); this is the extension point future semantic-value
 * classes (curves etc.) hook into instead of forking the transaction. */
gboolean dt_remote_patch_apply(const dt_introspection_field_t *linear,
                               const dt_remote_denylist_t *denylist,
                               const dt_remote_patch_t *patch,
                               void *params_blob,
                               dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* read API (live-module traversal)                                        */
/* ---------------------------------------------------------------------- */

/** snapshots the current view/image state. Always succeeds: `view` is the
 * current view's stable (untranslated) identifier, and when not in
 * darkroom or no image is open, `has_image` is FALSE with every image_*
 * field left at its zero/NULL value -- no domain errors, per the
 * protocol reference's error matrix. */
gboolean dt_remote_get_state(dt_remote_state_t **out,
                             dt_remote_error_t **error);

/** lists the live module instances of the current darkroom image, in
 * pipe order. Fails with DT_REMOTE_ERR_NOT_IN_DARKROOM /
 * DT_REMOTE_ERR_NO_IMAGE_OPEN as appropriate. */
gboolean dt_remote_list_modules(GPtrArray **out /* dt_remote_module_t */,
                                dt_remote_error_t **error);

/** returns only the primitive per-op parameter schema from the loaded
 * module .so. This internal lookup deliberately does not consult semantic
 * registries, so scalar mutation decoding remains available when semantic
 * metadata is invalid. Fails with DT_REMOTE_ERR_UNKNOWN_MODULE if `op` is
 * not loaded. */
gboolean dt_remote_get_module_primitive_schema(const char *op,
                                               dt_remote_module_schema_t **out,
                                               dt_remote_error_t **error);

/** returns the per-op parameter schema (shared by all instances of that
 * op), from the loaded module .so alone -- no darkroom/image needed.
 * Fails with DT_REMOTE_ERR_UNKNOWN_MODULE if `op` is not loaded. A
 * semantic registry/introspection mismatch fails closed with the original
 * DT_REMOTE_ERR_INTERNAL error, returning neither a primitive-only schema
 * nor semantic fields. */
gboolean dt_remote_get_module_schema(const char *op,
                                     dt_remote_module_schema_t **out,
                                     dt_remote_error_t **error);

/** reads back the current scalar values of a live module instance's
 * supported fields. Fails with DT_REMOTE_ERR_NOT_IN_DARKROOM /
 * DT_REMOTE_ERR_NO_IMAGE_OPEN / DT_REMOTE_ERR_UNKNOWN_MODULE /
 * DT_REMOTE_ERR_UNKNOWN_INSTANCE as appropriate. `semantic_out` may be
 * NULL to request only primitive values without consulting the semantic
 * registry. When non-NULL, it receives a newly allocated GHashTable of
 * name -> dt_remote_curve_value_t (free with g_hash_table_unref()); a
 * semantic read failure fails the whole call and leaves both outputs
 * untouched. */
gboolean dt_remote_get_module_params(const dt_remote_module_ref_t *ref,
                                     GPtrArray **out /* dt_remote_patch_entry_t */,
                                     GHashTable **semantic_out /* name -> dt_remote_curve_value_t */,
                                     dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* mutation API (plan step 7)                                              */
/* ---------------------------------------------------------------------- */

/** Atomically applies `patch` to a live module instance, per the mutation
 * engine sequence (internals doc §3, normative): checks the darkroom/
 * image precondition and, if `expected_revision` is non-NULL, the
 * compare-and-swap precondition against the current image
 * (DT_REMOTE_ERR_REVISION_CONFLICT on mismatch; the tracker's image
 * identity is self-healed via dt_remote_revision_observe_image() first,
 * so a revision the server previously handed out always matches against
 * unchanged state); locates the instance
 * (DT_REMOTE_ERR_UNKNOWN_MODULE / DT_REMOTE_ERR_UNKNOWN_INSTANCE); copies
 * the whole params block to scratch and runs dt_remote_patch_apply() over
 * it (any rejection there is returned unchanged, with live state
 * untouched -- the scratch copy is freed, never written back); on success,
 * copies the scratch block over the live params, applies
 * `patch->has_enable`'s tri-state to module->enabled in the same
 * transaction (parameters never implicitly enable a disabled module), and
 * commits with the preset-apply idiom (`dt_iop_gui_update()` then exactly
 * one `dt_dev_add_history_item()` call -- see internals doc §1's binding
 * rules and src/gui/presets.c's precedent). The resulting revision is the
 * tracker's counter read after dt_dev_add_history_item() returns: the
 * DEVELOP_HISTORY_CHANGE it raises is delivered synchronously on the GTK
 * main thread (g_main_context_invoke_full() invokes directly when the
 * calling thread owns the context), so the counter has already advanced;
 * if it did not (develop.c's gated/postponed raise paths), a plain
 * unpaired dt_remote_revision_force_bump() accounts for the change --
 * see that function's header comment for the fail-toward-retryable
 * design rule. `*out`'s fields are all read back from the now-live
 * module state, never echoed from `patch`.
 *
 * `expected_revision` may be NULL (no compare-and-swap: apply
 * unconditionally). Must be called on the GTK main thread (internals §1).
 * This is the live half of the transaction; dt_remote_patch_apply() above
 * is the pure half that unit tests exercise directly. */
gboolean dt_remote_set_module_params(const dt_remote_module_ref_t *ref,
                                     const dt_remote_patch_t *patch,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out,
                                     dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* mutation API (plan step 8)                                             */
/* ---------------------------------------------------------------------- */

/** Enables or disables a live module instance through the same path the
 * on/off header toggle uses (module->enabled = value, then exactly one
 * dt_dev_add_history_item()), sharing the whole revision/CAS/main-thread
 * discipline of dt_remote_set_module_params(): self-heal the tracker's
 * image identity, compare-and-swap against `expected_revision` when
 * non-NULL (DT_REMOTE_ERR_REVISION_CONFLICT on mismatch, nothing changed),
 * locate the instance (DT_REMOTE_ERR_UNKNOWN_MODULE /
 * DT_REMOTE_ERR_UNKNOWN_INSTANCE), toggle, commit, and read the resulting
 * revision back. `*out`'s `values` is NULL (enable carries no values on the
 * wire); `enabled` is read back from live state. Enabling is explicit and
 * always its own single history item -- it never rides on a params patch.
 * Must be called on the GTK main thread. */
gboolean dt_remote_set_module_enabled(const dt_remote_module_ref_t *ref,
                                      gboolean enabled,
                                      const uint64_t *expected_revision,
                                      dt_remote_mutation_result_t **out,
                                      dt_remote_error_t **error);

/** Restores a live module instance to its defaults through darktable's
 * normal reset lifecycle (the same core the reset-button callback runs:
 * drop any drawn mask, dt_iop_reload_defaults() to reload image-specific
 * default params + blend params, dt_iop_gui_reset()/dt_iop_gui_update() to
 * resync the widgets, then exactly one dt_dev_add_history_item()). Shares
 * the revision/CAS/main-thread discipline of the other mutations. `*out`'s
 * `values` holds the post-reset scalar values of every supported field
 * (matching the wire result), `enabled` is read back from live state. Must
 * be called on the GTK main thread. */
gboolean dt_remote_reset_module(const dt_remote_module_ref_t *ref,
                                const uint64_t *expected_revision,
                                dt_remote_mutation_result_t **out,
                                dt_remote_error_t **error);

/** Creates a new instance of a multi-instance module by delegating to the
 * native darkroom helper dt_iop_gui_duplicate(base, copy_params), which
 * builds the module, places its GUI expander, records BOTH history entries,
 * rebuilds the pixelpipe, and focuses it -- exactly the GUI new-instance /
 * duplicate button. `ref` addresses the source instance (its op, and
 * `instance` = source multi_priority). Rejects a module flagged
 * IOP_FLAGS_ONE_INSTANCE with DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED (nothing
 * created); an unknown op/source instance fails with
 * DT_REMOTE_ERR_UNKNOWN_MODULE / DT_REMOTE_ERR_UNKNOWN_INSTANCE. Shares the
 * revision/CAS/main-thread discipline of the other mutations; the returned
 * revision reflects the final state after both history entries. `*out`
 * carries the NEW instance's multi_priority (`instance`), read-back
 * multi_name (`instance_name`), and `enabled`; `values` is NULL (create
 * carries no values on the wire). Must be called on the GTK main thread. */
gboolean dt_remote_create_module_instance(const dt_remote_module_ref_t *ref,
                                          gboolean copy_params,
                                          const uint64_t *expected_revision,
                                          dt_remote_mutation_result_t **out,
                                          dt_remote_error_t **error);

/** Reads the darkroom history stack as model-oriented metadata only: one
 * dt_remote_history_item_t per entry (seq = stack position, op, instance,
 * translated display_name/instance_name, enabled) -- NO binary param
 * blobs. Items are ordered oldest -> newest; when the stack is longer than
 * `limit`, only the newest `limit` entries are returned (each keeping its
 * true stack position in `seq`). `limit` must already be clamped to
 * [1,100] by the caller. `*revision_out` receives the current coherent
 * revision (self-healed, like get_state). Fails with
 * DT_REMOTE_ERR_NOT_IN_DARKROOM / DT_REMOTE_ERR_NO_IMAGE_OPEN as
 * appropriate. Must be called on the GTK main thread. */
gboolean dt_remote_get_history(int limit,
                               GPtrArray **items_out /* dt_remote_history_item_t */,
                               uint64_t *revision_out,
                               dt_remote_error_t **error);

/** Compare-and-undo: if the live revision equals `expected_revision`, undo
 * exactly one history transition through darktable's undo system (the same
 * dt_undo_do_undo(darktable.undo, DT_UNDO_DEVELOP) entry point as Ctrl+Z),
 * otherwise fail with DT_REMOTE_ERR_REVISION_CONFLICT and change nothing.
 * `expected_revision` is required (there is no unconditional undo over the
 * protocol). Self-heals the tracker's image identity first, like every
 * mutation. `*revision_out` receives the new post-undo revision (undo is
 * itself a history change). Fails with DT_REMOTE_ERR_NOT_IN_DARKROOM /
 * DT_REMOTE_ERR_NO_IMAGE_OPEN as appropriate. Must be called on the GTK
 * main thread. */
gboolean dt_remote_undo(uint64_t expected_revision,
                        uint64_t *revision_out,
                        dt_remote_error_t **error);

/* ---------------------------------------------------------------------- */
/* preview rendering (plan step 9, internals §8)                           */
/* ---------------------------------------------------------------------- */

// What the main-thread prepare step captures for the background render:
// the darkroom image id and the revision stamped at the exact instant the
// live history was flushed to the database (the export path re-loads
// history from there -- internals §8's binding caveat).
typedef struct dt_remote_preview_request_t
{
  int32_t imgid;
  uint64_t revision;
} dt_remote_preview_request_t;

typedef struct dt_remote_preview_t   // <- render_preview (pre-base64)
{
  uint8_t *jpeg;        // owned (g_free); the encoded JPEG bytes
  size_t jpeg_len;
  int width, height;    // pixel dimensions of the encoded preview
  uint64_t revision;    // the revision actually rendered (from the request)
} dt_remote_preview_t;

/** frees a preview result, including its JPEG buffer. NULL-safe. */
void dt_remote_preview_free(dt_remote_preview_t *preview);

/** Returns the live process-local revision counter (self-healing singleton;
 * 0 when no server is connected). Read on the main thread at preview
 * completion to detect state drift between the pre-queue stamp and the
 * rendered result (internals §8's coherence check). */
uint64_t dt_remote_current_revision(void);

/** The main-thread half of render_preview (internals §8, binding): checks
 * the darkroom/image precondition (DT_REMOTE_ERR_NOT_IN_DARKROOM /
 * DT_REMOTE_ERR_NO_IMAGE_OPEN), flushes the live darkroom history to the
 * database with dt_dev_write_history() -- the export path re-loads history
 * from the DB, so without this flush the render would miss unsaved edits
 * -- and captures the image id and the revision at that same instant into
 * `*out`. The rendered preview is stamped with THAT revision. Must be
 * called on the GTK main thread, BEFORE queueing the render job. */
gboolean dt_remote_render_preview_prepare(dt_remote_preview_request_t *out,
                                          dt_remote_error_t **error);

/** The background half of render_preview: renders `req->imgid` through
 * dt_imageio_export_with_flags() with a synthetic in-memory format sink
 * (bpp 8, IMAGEIO_RGB|IMAGEIO_INT8, display_byteorder FALSE so the 8-bit
 * path emits RGBA -- exactly what dt_imageio_jpeg_compress() consumes) and
 * sRGB output color management (DT_COLORSPACE_SRGB: portable interchange;
 * the display profile would be wrong off-machine -- internals §8), bounded
 * to `max_px` on the longest edge (caller has already clamped it to
 * [64, 2048], and `quality` to [50, 95]), then JPEG-encodes in memory.
 *
 * `cancellable` (nullable) is polled at stage boundaries (before the
 * export and before the encode); once the pixelpipe run itself has
 * started it finishes -- cancellation then only saves the encode and the
 * result delivery. On cancellation or any render/encode failure returns
 * FALSE with DT_REMOTE_ERR_PREVIEW_FAILED and no buffer left allocated.
 *
 * This is deliberately the ONE remote_edit entry point that must NOT run
 * on the GTK main thread in production (internals §1: preview renders go
 * to background jobs): it never touches darktable.develop -- the export
 * builds its own dt_develop_t from the DB history flushed by prepare().
 * Requires a running darktable (mipmap cache, module system); covered by
 * the live integration harness, not unit tests. */
gboolean dt_remote_render_preview_execute(const dt_remote_preview_request_t *req,
                                          int max_px, int quality,
                                          GCancellable *cancellable,
                                          dt_remote_preview_t **out,
                                          dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
