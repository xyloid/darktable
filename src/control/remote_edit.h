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
} dt_remote_module_schema_t;

/** frees a module schema, including its fields. NULL-safe. */
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
  GPtrArray *semantic_values;  // reserved: semantic-class patches (curves
                               // etc., see the curve-classes design); NULL
                               // and unused in v1, but the transaction is
                               // written against this struct so semantic
                               // support extends it without a rewrite
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

/** returns the per-op parameter schema (shared by all instances of that
 * op), from the loaded module .so alone -- no darkroom/image needed.
 * Fails with DT_REMOTE_ERR_UNKNOWN_MODULE if `op` is not loaded. */
gboolean dt_remote_get_module_schema(const char *op,
                                     dt_remote_module_schema_t **out,
                                     dt_remote_error_t **error);

/** reads back the current scalar values of a live module instance's
 * supported fields. Fails with DT_REMOTE_ERR_NOT_IN_DARKROOM /
 * DT_REMOTE_ERR_NO_IMAGE_OPEN / DT_REMOTE_ERR_UNKNOWN_MODULE /
 * DT_REMOTE_ERR_UNKNOWN_INSTANCE as appropriate. */
gboolean dt_remote_get_module_params(const dt_remote_module_ref_t *ref,
                                     GPtrArray **out /* dt_remote_patch_entry_t */,
                                     dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
