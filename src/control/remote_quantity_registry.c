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

// The per-op quantity module adapter registry, mirroring
// remote_band_registry.c's own split: static adapter table, registry
// lifecycle (lookup/validate), and the read-only half of the quantity
// engine API (list_schema/read_values). The adapter table starts empty in
// this task -- Task 5 adds the `temperature` adapter (wb.temperature,
// Kelvin/tint), the same way lowlight/rawdenoise were the band registry's
// first real entries.
//
// This file also owns BOTH test-only override seams for the quantity
// engine: the lookup override (same idiom as every other class registry)
// AND the hooks override (new to this class -- replaces the module-owned
// conversion with a caller-supplied pair, since a quantity's read/write
// path is a hook call, not an introspection path resolve). The write half
// of the hooks override is shared with remote_quantity.c's mutation engine
// through a non-public accessor -- see that file's own top comment.
//
// Like remote_band_registry.c, this file never includes JSON, socket, or
// MCP protocol headers -- introspection/GLib (plus develop/imageop.h, for
// the dt_iop_module_so_t/dt_iop_module_t definitions the engine API's
// public signatures only forward-declare in remote_quantity.h) only.
//
// Threading: like remote_quantity.h/remote_band.h/remote_vector.h/
// remote_curve.h/remote_edit.h, resolving a path against a live darkroom
// module's params block, or calling a module's remote_quantity_read/write
// hook, must happen from the GTK main thread. The static registry table
// and the adapter/version intrinsic validation cache below is read-mostly,
// written once per (adapter, params_version) the first time that pair is
// validated; like the rest of this subsystem, this file assumes single
// (main-context) threaded access and adds no locking of its own.

#include "control/remote_quantity.h"

#include "common/darktable.h" // _()
#include "control/remote_band.h"   // dt_remote_band_registry_lookup(), cross-class uniqueness
#include "control/remote_vector.h" // dt_remote_vector_registry_lookup(), cross-class uniqueness
#include "develop/imageop.h" // dt_iop_module_so_t, dt_iop_module_t

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_band_registry.c's own file-local idiom --  */
/* see that file's comment)                                                */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_quantity_registry_error_new(dt_remote_error_code_t code,
                                                                  const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_quantity_registry_error_new(dt_remote_error_code_t code,
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

static void deliver_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

/* ---------------------------------------------------------------------- */
/* adapter table                                                           */
/* ---------------------------------------------------------------------- */

// Empty in this task -- Task 5 adds the first real entry (temperature),
// the same convention as remote_band_registry.c's own s_adapters[] before
// its first adapter landed: a placeholder NULL keeps the array
// well-formed C, and the lookup loop below already skips NULL entries.
static const dt_remote_quantity_module_adapter_t *const s_adapters[] = {
  NULL,
};

static dt_remote_quantity_registry_lookup_override_t s_lookup_override = NULL;

void dt_remote_quantity_registry_set_lookup_override(dt_remote_quantity_registry_lookup_override_t lookup)
{
  s_lookup_override = lookup;
}

// Hooks override storage: replaces the module-owned conversion with a
// caller-supplied pair for tests. `dt_remote_quantity_registry_current_write_hook()`
// exposes the write half to remote_quantity.c's mutation engine (not part
// of the public header -- see this file's own top comment); the read half
// is only ever consulted from this same file's dt_remote_quantity_read_values().
static dt_remote_quantity_read_hook_t s_read_hook_override = NULL;
static dt_remote_quantity_write_hook_t s_write_hook_override = NULL;

void dt_remote_quantity_set_hooks_override(dt_remote_quantity_read_hook_t read,
                                           dt_remote_quantity_write_hook_t write)
{
  s_read_hook_override = read;
  s_write_hook_override = write;
}

dt_remote_quantity_write_hook_t dt_remote_quantity_registry_current_write_hook(void)
{
  return s_write_hook_override;
}

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

const dt_remote_quantity_module_adapter_t *
dt_remote_quantity_registry_lookup(const char *operation, guint params_version)
{
  if(!operation) return NULL;
  if(s_lookup_override) return s_lookup_override(operation, params_version);

  for(guint i = 0; i < G_N_ELEMENTS(s_adapters); i++)
  {
    const dt_remote_quantity_module_adapter_t *adapter = s_adapters[i];
    if(!adapter) continue;
    if(!g_strcmp0(adapter->operation, operation)
       && params_version >= adapter->minimum_params_version
       && params_version <= adapter->maximum_params_version)
      return adapter;
  }

  return NULL;
}

// Process-lifetime intrinsic validation cache, keyed by adapter pointer
// identity and the introspection params version -- same convention as
// remote_band_registry.c's own cache. Cross-class uniqueness and hook
// presence are both mutable under process-wide state (the registry
// overrides and the hooks override, respectively) and are therefore not
// cached.
typedef struct dt_remote_quantity_registry_cache_key_t
{
  const dt_remote_quantity_module_adapter_t *adapter;
  int params_version;
} dt_remote_quantity_registry_cache_key_t;

typedef struct dt_remote_quantity_registry_cache_entry_t
{
  gboolean adapter_valid; // TRUE iff every intrinsic check passed for this version
} dt_remote_quantity_registry_cache_entry_t;

static GHashTable *s_validation_cache = NULL; // cache key -> intrinsic result; never freed
                                              // (process lifetime, like the static descriptors/
                                              // adapters it caches results for)

static guint validation_cache_key_hash(gconstpointer data)
{
  const dt_remote_quantity_registry_cache_key_t *key = data;
  return g_direct_hash((gpointer)key->adapter) ^ g_int_hash(&key->params_version);
}

static gboolean validation_cache_key_equal(gconstpointer left, gconstpointer right)
{
  const dt_remote_quantity_registry_cache_key_t *a = left;
  const dt_remote_quantity_registry_cache_key_t *b = right;
  return a->adapter == b->adapter && a->params_version == b->params_version;
}

static dt_remote_quantity_registry_cache_entry_t *
lookup_cache_entry(const dt_remote_quantity_module_adapter_t *adapter, int params_version)
{
  if(!s_validation_cache)
    s_validation_cache = g_hash_table_new_full(validation_cache_key_hash, validation_cache_key_equal,
                                               g_free, g_free);

  const dt_remote_quantity_registry_cache_key_t key = {
    .adapter = adapter,
    .params_version = params_version,
  };
  return g_hash_table_lookup(s_validation_cache, &key);
}

static void cache_validation_result(const dt_remote_quantity_module_adapter_t *adapter,
                                    int params_version,
                                    gboolean adapter_valid)
{
  dt_remote_quantity_registry_cache_key_t *key = g_new(dt_remote_quantity_registry_cache_key_t, 1);
  key->adapter = adapter;
  key->params_version = params_version;

  dt_remote_quantity_registry_cache_entry_t *entry = g_new(dt_remote_quantity_registry_cache_entry_t, 1);
  entry->adapter_valid = adapter_valid;
  g_hash_table_insert(s_validation_cache, key, entry);
}

static gboolean nonempty_string(const char *value)
{
  return value && value[0] != '\0';
}

static gboolean quantity_adapter_envelope_is_valid(
  const dt_remote_quantity_module_adapter_t *adapter,
  const dt_iop_module_so_t *so,
  const dt_introspection_t *intro)
{
  if(!nonempty_string(adapter->operation)
     || g_strcmp0(adapter->operation, so->op)
     || adapter->minimum_params_version > adapter->maximum_params_version
     || intro->params_version < 0
     || (guint)intro->params_version < adapter->minimum_params_version
     || (guint)intro->params_version > adapter->maximum_params_version
     || (adapter->quantity_count > 0 && !adapter->quantities)
     || (adapter->native_field_count > 0 && !adapter->native_fields))
    return FALSE;

  for(guint i = 0; i < adapter->native_field_count; i++)
    if(!nonempty_string(adapter->native_fields[i])) return FALSE;
  return TRUE;
}

static gboolean quantity_component_metadata_is_valid(const dt_remote_quantity_descriptor_t *desc)
{
  if(!nonempty_string(desc->name)
     || desc->component_count == 0
     || desc->component_count > DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP
     || !desc->components)
    return FALSE;

  for(guint i = 0; i < desc->component_count; i++)
  {
    const dt_remote_quantity_component_descriptor_t *component = &desc->components[i];
    if(!nonempty_string(component->name)
       || !isfinite(component->minimum)
       || !isfinite(component->maximum)
       || component->minimum >= component->maximum)
      return FALSE;
  }
  return TRUE;
}

static gboolean quantity_predicate_is_valid(const dt_remote_parameter_predicate_t *predicate,
                                            const dt_introspection_field_t *root,
                                            void *dummy_blob)
{
  if(!predicate) return TRUE;

  if(predicate->op != DT_REMOTE_PREDICATE_EQ && predicate->op != DT_REMOTE_PREDICATE_NE)
    return FALSE;

  dt_introspection_field_t *field = NULL;
  if(!dt_introspection_get_child((dt_introspection_field_t *)root, dummy_blob,
                                 predicate->field, &field)
     || !field
     || field->header.type != DT_INTROSPECTION_TYPE_ENUM
     || !field->Enum.values)
    return FALSE;

  int unused_value = 0;
  return dt_introspection_get_enum_value(field, predicate->enum_name, &unused_value);
}

// Checks one descriptor's metadata and optional predicates against `root`
// (the module's whole params struct field, DT_INTROSPECTION_TYPE_STRUCT).
// A quantity descriptor has no native introspection path of its own (see
// remote_quantity.h's own top comment) -- there is nothing to resolve here
// beyond the predicates. `dummy_blob` exists only to satisfy the
// introspection cursor signatures; validation never reads its pointee.
static gboolean quantity_descriptor_is_valid(const dt_remote_quantity_descriptor_t *desc,
                                             const dt_introspection_field_t *root,
                                             void *dummy_blob)
{
  if(!quantity_component_metadata_is_valid(desc)) return FALSE;
  if(root->header.type != DT_INTROSPECTION_TYPE_STRUCT) return FALSE;

  return quantity_predicate_is_valid(desc->active_when, root, dummy_blob)
         && quantity_predicate_is_valid(desc->writable_when, root, dummy_blob);
}

static gboolean quantity_adapter_has_duplicate_names(const dt_remote_quantity_module_adapter_t *adapter,
                                                      const char **out_name)
{
  for(guint i = 0; i < adapter->quantity_count; i++)
    for(guint j = 0; j < i; j++)
      if(!g_strcmp0(adapter->quantities[i].name, adapter->quantities[j].name))
      {
        if(out_name) *out_name = adapter->quantities[i].name;
        return TRUE;
      }
  return FALSE;
}

// Every `adapter->native_fields` name must resolve against `root` to a
// writable (not on the per-op denylist) scalar FLOAT leaf -- the write
// hook's declared conflict/byte-check set.
static gboolean quantity_native_fields_are_valid(const dt_remote_quantity_module_adapter_t *adapter,
                                                 const dt_iop_module_so_t *so,
                                                 const dt_introspection_field_t *root,
                                                 void *dummy_blob)
{
  const dt_remote_denylist_t *denylist = dt_remote_denylist_for_op(so->op);

  for(guint i = 0; i < adapter->native_field_count; i++)
  {
    const char *name = adapter->native_fields[i];
    if(dt_remote_denylisted(denylist, name)) return FALSE;

    const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = name };
    const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
    const dt_introspection_field_t *field = NULL;
    if(!dt_remote_path_resolve(&path, root, dummy_blob, &field, NULL, NULL)) return FALSE;
    if(!field || field->header.type != DT_INTROSPECTION_TYPE_FLOAT) return FALSE;
  }
  return TRUE;
}

// Cross-class uniqueness: no quantity semantic ID on `adapter` may collide
// with a curve, vector, or band semantic ID registered for the same
// (operation, params_version) pair. Returns TRUE (and, if non-NULL, sets
// *out_collision_name) on the first collision found.
static gboolean quantity_names_collide_with_other_classes(
  const dt_remote_quantity_module_adapter_t *adapter,
  guint params_version,
  const char **out_collision_name)
{
  const dt_remote_curve_module_adapter_t *curve_adapter =
    dt_remote_curve_registry_lookup(adapter->operation, params_version);
  if(curve_adapter)
    for(guint i = 0; i < adapter->quantity_count; i++)
      for(guint j = 0; j < curve_adapter->curve_count; j++)
        if(!g_strcmp0(adapter->quantities[i].name, curve_adapter->curves[j].name))
        {
          if(out_collision_name) *out_collision_name = adapter->quantities[i].name;
          return TRUE;
        }

  const dt_remote_vector_module_adapter_t *vector_adapter =
    dt_remote_vector_registry_lookup(adapter->operation, params_version);
  if(vector_adapter)
    for(guint i = 0; i < adapter->quantity_count; i++)
      for(guint j = 0; j < vector_adapter->vector_count; j++)
        if(!g_strcmp0(adapter->quantities[i].name, vector_adapter->vectors[j].name))
        {
          if(out_collision_name) *out_collision_name = adapter->quantities[i].name;
          return TRUE;
        }

  const dt_remote_band_module_adapter_t *band_adapter =
    dt_remote_band_registry_lookup(adapter->operation, params_version);
  if(band_adapter)
    for(guint i = 0; i < adapter->quantity_count; i++)
      for(guint j = 0; j < band_adapter->band_count; j++)
        if(!g_strcmp0(adapter->quantities[i].name, band_adapter->bands[j].name))
        {
          if(out_collision_name) *out_collision_name = adapter->quantities[i].name;
          return TRUE;
        }

  return FALSE;
}

gboolean dt_remote_quantity_registry_validate(const dt_remote_quantity_module_adapter_t *adapter,
                                              const struct dt_iop_module_so_t *so,
                                              dt_remote_error_t **error)
{
  if(!adapter || !so)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: null adapter or module passed to quantity registry validation"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  if(!quantity_adapter_envelope_is_valid(adapter, so, intro))
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("quantity adapter metadata does not match module '%s' introspection version %d"),
        so->op, intro->params_version);
    return FALSE;
  }

  dt_remote_quantity_registry_cache_entry_t *cache =
    lookup_cache_entry(adapter, intro->params_version);
  if(!cache)
  {
    gboolean intrinsic_valid = TRUE;
    const char *first_failure = NULL;
    if(quantity_adapter_has_duplicate_names(adapter, &first_failure)) intrinsic_valid = FALSE;
    guint8 dummy_byte = 0;
    for(guint i = 0; i < adapter->quantity_count; i++)
    {
      const dt_remote_quantity_descriptor_t *desc = &adapter->quantities[i];
      if(!quantity_descriptor_is_valid(desc, intro->field, &dummy_byte))
      {
        intrinsic_valid = FALSE;
        if(!first_failure) first_failure = desc->name;
      }
    }
    if(!quantity_native_fields_are_valid(adapter, so, intro->field, &dummy_byte))
      intrinsic_valid = FALSE;

    cache_validation_result(adapter, intro->params_version, intrinsic_valid);
    cache = lookup_cache_entry(adapter, intro->params_version);

    if(!intrinsic_valid)
    {
      deliver_error(dt_remote_quantity_registry_error_new(
                      DT_REMOTE_ERR_INTERNAL,
                      _("quantity adapter '%s' descriptor '%s' does not match introspection version %d"),
                      adapter->operation, first_failure ? first_failure : "", intro->params_version),
                    error);
      return FALSE;
    }
  }

  if(!cache->adapter_valid)
  {
    deliver_error(dt_remote_quantity_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("quantity adapter '%s' does not match introspection version %d"),
                    adapter->operation, intro->params_version),
                  error);
    return FALSE;
  }

  // Hook presence: not cached, since it depends on the process-wide hooks
  // override state, not on static metadata or introspection. Production
  // code never installs an override, so this always resolves to the real
  // so-level check there.
  const gboolean override_installed = s_read_hook_override != NULL || s_write_hook_override != NULL;
  if(!override_installed && (!so->remote_quantity_read || !so->remote_quantity_write))
  {
    deliver_error(dt_remote_quantity_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("module '%s' does not implement the remote_quantity_read/write hooks"), so->op),
                  error);
    return FALSE;
  }

  const char *collision_name = NULL;
  if(quantity_names_collide_with_other_classes(adapter, (guint)intro->params_version, &collision_name))
  {
    deliver_error(dt_remote_quantity_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("semantic quantity '%s' collides with a curve, vector, or band ID"),
                    collision_name ? collision_name : ""),
                  error);
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* quantity engine API -- read-only half (list_schema/read_values)         */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *condition_from_predicate(const dt_remote_parameter_predicate_t *pred)
{
  if(!pred) return NULL;
  dt_remote_parameter_condition_t *condition = g_new0(dt_remote_parameter_condition_t, 1);
  condition->field = g_strdup(pred->field);
  condition->op = pred->op;
  condition->enum_name = g_strdup(pred->enum_name);
  return condition;
}

gboolean dt_remote_quantity_list_schema(const struct dt_iop_module_so_t *so,
                                        GPtrArray **out_fields,
                                        dt_remote_error_t **error)
{
  if(out_fields) *out_fields = NULL;
  if(!so || !out_fields)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to quantity schema listing"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  const dt_remote_quantity_module_adapter_t *adapter =
    dt_remote_quantity_registry_lookup(so->op, (guint)intro->params_version);
  if(!adapter)
  {
    // No adapter for this op/version: no derived quantity advertised, not
    // an error.
    *out_fields = NULL;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_quantity_registry_validate(adapter, so, &validate_error);
  if(!adapter_valid)
  {
    if(!validate_error)
      validate_error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: quantity registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_quantity_schema_free);

  for(guint i = 0; i < adapter->quantity_count; i++)
  {
    const dt_remote_quantity_descriptor_t *desc = &adapter->quantities[i];
    dt_remote_quantity_schema_t *schema = g_new0(dt_remote_quantity_schema_t, 1);

    schema->name = g_strdup(desc->name);
    schema->display_name = g_strdup(desc->display_name ? desc->display_name : "");
    schema->description = desc->description ? g_strdup(desc->description) : NULL;
    schema->derived = desc->derived;
    schema->writability = desc->writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NOW;
    schema->active_when = condition_from_predicate(desc->active_when);
    schema->writable_when = condition_from_predicate(desc->writable_when);

    // `components` is a plain GPtrArray with no attached element destructor
    // -- dt_remote_quantity_schema_free() frees each element explicitly
    // before unref'ing the array, same idiom as remote_vector.c's
    // dt_remote_vector_schema_free() manual loop over its GArray of
    // component structs.
    GPtrArray *components = g_ptr_array_new();
    for(guint c = 0; c < desc->component_count; c++)
    {
      const dt_remote_quantity_component_descriptor_t *component = &desc->components[c];
      dt_remote_quantity_schema_component_t *component_schema =
        g_new0(dt_remote_quantity_schema_component_t, 1);
      component_schema->name = g_strdup(component->name);
      component_schema->unit = component->unit ? g_strdup(component->unit) : NULL;
      component_schema->minimum = component->minimum;
      component_schema->maximum = component->maximum;
      g_ptr_array_add(components, component_schema);
    }
    schema->components = components;

    g_ptr_array_add(result, schema);
  }

  *out_fields = result;
  return TRUE;
}

// Evaluates `pred` against `params_blob`, resolving `pred->field` as a one
// segment path off `root` and comparing by enum *name*, never by raw
// integer -- same contract as remote_band_registry.c's own
// predicate_holds() (this file keeps its own copy rather than sharing a
// static function across translation units, same established idiom as the
// curve/vector/band twins).
static gboolean predicate_holds(const dt_remote_parameter_predicate_t *pred,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                gboolean *out_holds,
                                dt_remote_error_t **error)
{
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = pred->field };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };

  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  dt_remote_error_t *resolve_error = NULL;
  if(!dt_remote_path_resolve(&path, root, params_blob, &field, &ptr, &resolve_error))
  {
    if(!resolve_error)
      resolve_error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: quantity predicate field '%s' did not resolve"),
        pred->field);
    deliver_error(resolve_error, error);
    return FALSE;
  }
  if(field->header.type != DT_INTROSPECTION_TYPE_ENUM)
  {
    deliver_error(dt_remote_quantity_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: quantity predicate field '%s' is not an enum"), pred->field),
                  error);
    return FALSE;
  }

  const int value = *(const int *)ptr;
  const char *name = dt_introspection_get_enum_name((dt_introspection_field_t *)field, value);
  if(!name)
  {
    deliver_error(dt_remote_quantity_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: quantity predicate field '%s' has unknown enum value %d"),
                    pred->field, value),
                  error);
    return FALSE;
  }

  const gboolean matches = !g_strcmp0(name, pred->enum_name);
  *out_holds = (pred->op == DT_REMOTE_PREDICATE_EQ) ? matches : !matches;
  return TRUE;
}

gboolean dt_remote_quantity_read_values(const struct dt_iop_module_t *module,
                                        const void *params,
                                        GHashTable **out,
                                        dt_remote_error_t **error)
{
  if(out) *out = NULL;
  if(!module || !params || !out)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to quantity value read"));
    return FALSE;
  }

  if(!module->so)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module instance has no .so"));
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), module->op);
    return FALSE;
  }

  GHashTable *result = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)dt_remote_quantity_value_free);

  const dt_remote_quantity_module_adapter_t *adapter =
    dt_remote_quantity_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    *out = result;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_quantity_registry_validate(adapter, module->so, &validate_error);
  if(!adapter_valid)
  {
    g_hash_table_destroy(result);
    if(!validate_error)
      validate_error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: quantity registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  void *params_blob = (void *)params;
  struct dt_iop_module_t *mutable_module = (struct dt_iop_module_t *)module;
  dt_remote_quantity_read_hook_t hook = s_read_hook_override ? s_read_hook_override
                                                             : module->so->remote_quantity_read;
  dt_remote_error_t *read_error = NULL;

  for(guint i = 0; i < adapter->quantity_count; i++)
  {
    const dt_remote_quantity_descriptor_t *desc = &adapter->quantities[i];

    gboolean active = TRUE;
    if(desc->active_when
       && !predicate_holds(desc->active_when, intro->field, params_blob, &active, &read_error))
      goto fail;

    gboolean writable_now = TRUE;
    if(desc->writable_when
       && !predicate_holds(desc->writable_when, intro->field, params_blob, &writable_now, &read_error))
      goto fail;

    if(!hook)
    {
      read_error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: module '%s' has no quantity read hook"), module->op);
      goto fail;
    }

    double *values_buf = g_new(double, desc->component_count);
    const gboolean read_ok =
      hook(mutable_module, params_blob, values_buf, desc->component_count);
    if(!read_ok)
    {
      g_free(values_buf);
      read_error = dt_remote_quantity_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: quantity '%s' conversion read hook failed"), desc->name);
      goto fail;
    }

    dt_remote_quantity_value_t *value = g_new0(dt_remote_quantity_value_t, 1);
    value->name = g_strdup(desc->name);
    value->values =
      g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_quantity_component_value_free);
    for(guint c = 0; c < desc->component_count; c++)
    {
      dt_remote_quantity_component_value_t *component_value =
        g_new0(dt_remote_quantity_component_value_t, 1);
      component_value->name = g_strdup(desc->components[c].name);
      component_value->value = values_buf[c];
      g_ptr_array_add(value->values, component_value);
    }
    g_free(values_buf);
    value->active = active;
    value->effective = active;
    value->writable_now = writable_now;

    g_hash_table_insert(result, g_strdup(desc->name), value);
  }

  *out = result;
  return TRUE;

fail:
  g_hash_table_destroy(result);
  if(!read_error)
    read_error = dt_remote_quantity_registry_error_new(
      DT_REMOTE_ERR_INTERNAL, _("internal error: quantity value read failed"));
  deliver_error(read_error, error);
  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
