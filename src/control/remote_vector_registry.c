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

// The per-op vector module adapter registry, mirroring
// remote_curve_registry.c's own split: static adapter table, registry
// lifecycle (lookup/validate), and the read-only half of the vector engine
// API (list_schema/read_values). The adapter table starts empty in this
// task -- later tasks (color/levels/plain vector adapters) populate it, the
// same way rgbcurve/tonecurve/colorzones/basecurve were added to the curve
// registry one at a time.
//
// Like remote_vector.c, this file never includes JSON, socket, or MCP
// protocol headers -- introspection/GLib (plus develop/imageop.h, for the
// dt_iop_module_so_t/dt_iop_module_t definitions the engine API's public
// signatures only forward-declare in remote_vector.h) only.
//
// Threading: like remote_vector.h/remote_curve.h/remote_edit.h, resolving a
// path against a live darkroom module's params block must happen from the
// GTK main thread. The static registry table and the adapter/version
// validation cache below are read-mostly, written once per (adapter,
// params_version) the first time that pair is validated; like the rest of
// this subsystem, this file assumes single (main-context) threaded access
// and adds no locking of its own.

#include "control/remote_vector.h"

#include "common/darktable.h" // _()
#include "develop/imageop.h" // dt_iop_module_so_t, dt_iop_module_t

#include <math.h>
#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_vector.c's own file-local idiom -- see     */
/* that file's comment)                                                    */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_vector_registry_error_new(dt_remote_error_code_t code,
                                                                const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_vector_registry_error_new(dt_remote_error_code_t code,
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
/* adapter table (empty; Tasks 5-8 populate it)                           */
/* ---------------------------------------------------------------------- */

// No adapters registered yet. dt_remote_vector_registry_lookup() therefore
// returns NULL for every real operation until a later task adds an entry
// here -- the "no adapter" paths in list_schema/read_values/apply_patch
// (silent degrade) are the ones exercised in production right now. The
// lone NULL keeps this a valid, non-empty C array initializer; the lookup
// loop below skips it.
static const dt_remote_vector_module_adapter_t *const s_adapters[] = { NULL };

static dt_remote_vector_registry_lookup_override_t s_lookup_override = NULL;

void dt_remote_vector_registry_set_lookup_override(dt_remote_vector_registry_lookup_override_t lookup)
{
  s_lookup_override = lookup;
}

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

const dt_remote_vector_module_adapter_t *
dt_remote_vector_registry_lookup(const char *operation, guint params_version)
{
  if(!operation) return NULL;
  if(s_lookup_override) return s_lookup_override(operation, params_version);

  for(guint i = 0; i < G_N_ELEMENTS(s_adapters); i++)
  {
    const dt_remote_vector_module_adapter_t *adapter = s_adapters[i];
    if(!adapter) continue;
    if(!g_strcmp0(adapter->operation, operation)
       && params_version >= adapter->minimum_params_version
       && params_version <= adapter->maximum_params_version)
      return adapter;
  }

  return NULL;
}

// Process-lifetime validation cache, keyed by adapter pointer identity and
// the introspection params version -- same convention as
// remote_curve_registry.c's own cache.
typedef struct dt_remote_vector_registry_cache_key_t
{
  const dt_remote_vector_module_adapter_t *adapter;
  int params_version;
} dt_remote_vector_registry_cache_key_t;

typedef struct dt_remote_vector_registry_cache_entry_t
{
  gboolean adapter_valid; // TRUE iff every check passed for this version
} dt_remote_vector_registry_cache_entry_t;

static GHashTable *s_validation_cache = NULL; // cache key -> aggregate result; never freed
                                              // (process lifetime, like the static
                                              // descriptors/adapters it caches results for)

static guint validation_cache_key_hash(gconstpointer data)
{
  const dt_remote_vector_registry_cache_key_t *key = data;
  return g_direct_hash((gpointer)key->adapter) ^ g_int_hash(&key->params_version);
}

static gboolean validation_cache_key_equal(gconstpointer left, gconstpointer right)
{
  const dt_remote_vector_registry_cache_key_t *a = left;
  const dt_remote_vector_registry_cache_key_t *b = right;
  return a->adapter == b->adapter && a->params_version == b->params_version;
}

static dt_remote_vector_registry_cache_entry_t *
lookup_cache_entry(const dt_remote_vector_module_adapter_t *adapter, int params_version)
{
  if(!s_validation_cache)
    s_validation_cache = g_hash_table_new_full(validation_cache_key_hash, validation_cache_key_equal,
                                               g_free, g_free);

  const dt_remote_vector_registry_cache_key_t key = {
    .adapter = adapter,
    .params_version = params_version,
  };
  return g_hash_table_lookup(s_validation_cache, &key);
}

static void cache_validation_result(const dt_remote_vector_module_adapter_t *adapter,
                                    int params_version,
                                    gboolean adapter_valid)
{
  dt_remote_vector_registry_cache_key_t *key = g_new(dt_remote_vector_registry_cache_key_t, 1);
  key->adapter = adapter;
  key->params_version = params_version;

  dt_remote_vector_registry_cache_entry_t *entry = g_new(dt_remote_vector_registry_cache_entry_t, 1);
  entry->adapter_valid = adapter_valid;
  g_hash_table_insert(s_validation_cache, key, entry);
}

static gboolean vector_predicate_is_valid(const dt_remote_parameter_predicate_t *predicate,
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

static gboolean nonempty_string(const char *value)
{
  return value && value[0] != '\0';
}

static gboolean vector_adapter_envelope_is_valid(
  const dt_remote_vector_module_adapter_t *adapter,
  const dt_iop_module_so_t *so,
  const dt_introspection_t *intro)
{
  if(!nonempty_string(adapter->operation)
     || g_strcmp0(adapter->operation, so->op)
     || adapter->minimum_params_version > adapter->maximum_params_version
     || intro->params_version < 0
     || (guint)intro->params_version < adapter->minimum_params_version
     || (guint)intro->params_version > adapter->maximum_params_version
     || (adapter->vector_count > 0 && !adapter->vectors)
     || (adapter->prepare_field_count > 0 && !adapter->prepare_fields))
    return FALSE;

  for(guint i = 0; i < adapter->prepare_field_count; i++)
    if(!nonempty_string(adapter->prepare_fields[i])) return FALSE;
  return TRUE;
}

static gboolean vector_component_metadata_is_valid(
  const dt_remote_vector_descriptor_t *desc)
{
  if(!nonempty_string(desc->name) || desc->component_count == 0 || !desc->components)
    return FALSE;

  for(guint i = 0; i < desc->component_count; i++)
  {
    const dt_remote_vector_component_t *component = &desc->components[i];
    if(!nonempty_string(component->name)
       || !isfinite(component->minimum)
       || !isfinite(component->maximum)
       || component->minimum > component->maximum)
      return FALSE;
  }
  return TRUE;
}

static gboolean vector_subtype_metadata_is_valid(
  const dt_remote_vector_descriptor_t *desc)
{
  switch(desc->subtype)
  {
    case DT_REMOTE_VECTOR_PLAIN:
      return !desc->color_space && !desc->strictly_increasing && desc->minimum_gap == 0.0;
    case DT_REMOTE_VECTOR_COLOR:
      return nonempty_string(desc->color_space)
             && !desc->strictly_increasing && desc->minimum_gap == 0.0;
    case DT_REMOTE_VECTOR_LEVELS:
      return !desc->color_space && desc->strictly_increasing
             && isfinite(desc->minimum_gap) && desc->minimum_gap >= 0.0;
    default:
      return FALSE;
  }
}

// Checks one descriptor's native layout (a single fixed-capacity float
// array leaf -- never a struct-of-nodes array like a curve's) and optional
// predicates against `root` (the module's whole params struct field,
// DT_INTROSPECTION_TYPE_STRUCT). `dummy_blob` exists only to satisfy the
// introspection cursor signatures; validation never reads its pointee.
static gboolean vector_descriptor_is_valid(const dt_remote_vector_descriptor_t *desc,
                                           const dt_introspection_field_t *root,
                                           void *dummy_blob)
{
  if(!vector_component_metadata_is_valid(desc)) return FALSE;
  if(!vector_subtype_metadata_is_valid(desc)) return FALSE;
  if(root->header.type != DT_INTROSPECTION_TYPE_STRUCT) return FALSE;

  if(desc->native.length && !desc->native.segments) return FALSE;

  const dt_introspection_field_t *array_field = NULL;
  if(!dt_remote_path_resolve(&desc->native, root, dummy_blob, &array_field, NULL, NULL))
    return FALSE;
  if(!array_field
     || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT
     || !array_field->Array.field
     || array_field->Array.field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || array_field->Array.field->header.size != sizeof(float))
    return FALSE;
  if(array_field->Array.count < (size_t)desc->component_count) return FALSE;
  if(array_field->Array.count != (size_t)desc->native_capacity) return FALSE;

  return vector_predicate_is_valid(desc->active_when, root, dummy_blob)
         && vector_predicate_is_valid(desc->writable_when, root, dummy_blob);
}

// Cross-class uniqueness: no vector semantic ID on `adapter` may collide
// with a curve semantic ID registered for the same (operation,
// params_version) pair. Returns TRUE (and, if non-NULL, sets
// *out_collision_name) on the first collision found.
static gboolean vector_names_collide_with_curve(const dt_remote_vector_module_adapter_t *adapter,
                                                guint params_version,
                                                const char **out_collision_name)
{
  const dt_remote_curve_module_adapter_t *curve_adapter =
    dt_remote_curve_registry_lookup(adapter->operation, params_version);
  if(!curve_adapter) return FALSE;

  for(guint i = 0; i < adapter->vector_count; i++)
    for(guint j = 0; j < curve_adapter->curve_count; j++)
      if(!g_strcmp0(adapter->vectors[i].name, curve_adapter->curves[j].name))
      {
        if(out_collision_name) *out_collision_name = adapter->vectors[i].name;
        return TRUE;
      }
  return FALSE;
}

static gboolean vector_adapter_has_duplicate_names(const dt_remote_vector_module_adapter_t *adapter,
                                                    const char **out_name)
{
  for(guint i = 0; i < adapter->vector_count; i++)
    for(guint j = 0; j < i; j++)
      if(!g_strcmp0(adapter->vectors[i].name, adapter->vectors[j].name))
      {
        if(out_name) *out_name = adapter->vectors[i].name;
        return TRUE;
      }
  return FALSE;
}

gboolean dt_remote_vector_registry_validate(const dt_remote_vector_module_adapter_t *adapter,
                                            const struct dt_iop_module_so_t *so,
                                            dt_remote_error_t **error)
{
  if(!adapter || !so)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: null adapter or module passed to vector registry validation"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  if(!vector_adapter_envelope_is_valid(adapter, so, intro))
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("vector adapter metadata does not match module '%s' introspection version %d"),
        so->op, intro->params_version);
    return FALSE;
  }

  dt_remote_vector_registry_cache_entry_t *cache =
    lookup_cache_entry(adapter, intro->params_version);
  if(cache)
  {
    if(!cache->adapter_valid && error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("vector adapter '%s' has one or more descriptors that do not match introspection version %d"),
        adapter->operation ? adapter->operation : "", intro->params_version);
    return cache->adapter_valid;
  }

  gboolean all_valid = TRUE;
  const char *first_failure = NULL;

  const char *duplicate_name = NULL;
  if(vector_adapter_has_duplicate_names(adapter, &duplicate_name))
  {
    all_valid = FALSE;
    first_failure = duplicate_name;
  }

  const char *collision_name = NULL;
  if(vector_names_collide_with_curve(adapter, (guint)intro->params_version, &collision_name))
  {
    all_valid = FALSE;
    if(!first_failure) first_failure = collision_name;
  }

  guint8 dummy_byte = 0;
  for(guint i = 0; i < adapter->vector_count; i++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[i];
    const gboolean ok = vector_descriptor_is_valid(desc, intro->field, &dummy_byte);
    if(!ok)
    {
      all_valid = FALSE;
      if(!first_failure) first_failure = desc->name;
    }
  }

  cache_validation_result(adapter, intro->params_version, all_valid);

  if(!all_valid)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("vector descriptor '%s' does not match introspection version %d"),
        first_failure ? first_failure : "", intro->params_version);
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* vector engine API -- read-only half (list_schema/read_values)           */
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

gboolean dt_remote_vector_list_schema(const struct dt_iop_module_so_t *so,
                                      GPtrArray **out_fields,
                                      dt_remote_error_t **error)
{
  if(out_fields) *out_fields = NULL;
  if(!so || !out_fields)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to vector schema listing"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  const dt_remote_vector_module_adapter_t *adapter =
    dt_remote_vector_registry_lookup(so->op, (guint)intro->params_version);
  if(!adapter)
  {
    // No adapter for this op/version: no semantic vector advertised, not
    // an error (native array fields simply remain unsupported elsewhere).
    *out_fields = NULL;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_vector_registry_validate(adapter, so, &validate_error);
  if(!adapter_valid)
  {
    if(!validate_error)
      validate_error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: vector registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_vector_schema_free);

  for(guint i = 0; i < adapter->vector_count; i++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[i];
    dt_remote_vector_schema_t *schema = g_new0(dt_remote_vector_schema_t, 1);

    schema->name = g_strdup(desc->name);
    schema->display_name = g_strdup(desc->display_name ? desc->display_name : "");
    schema->description = desc->description ? g_strdup(desc->description) : NULL;
    schema->subtype = desc->subtype;
    schema->color_space = desc->color_space ? g_strdup(desc->color_space) : NULL;
    schema->components =
      g_array_sized_new(FALSE, FALSE, sizeof(dt_remote_vector_component_schema_t), desc->component_count);
    for(guint j = 0; j < desc->component_count; j++)
    {
      const dt_remote_vector_component_schema_t component = {
        .name = g_strdup(desc->components[j].name),
        .minimum = desc->components[j].minimum,
        .maximum = desc->components[j].maximum,
      };
      g_array_append_val(schema->components, component);
    }
    schema->strictly_increasing = desc->strictly_increasing;
    schema->minimum_gap = desc->minimum_gap;
    schema->writability = desc->writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NOW;
    schema->active_when = condition_from_predicate(desc->active_when);
    schema->writable_when = condition_from_predicate(desc->writable_when);

    g_ptr_array_add(result, schema);
  }

  *out_fields = result;
  return TRUE;
}

static gboolean is_floating_type(dt_introspection_type_t type)
{
  return type == DT_INTROSPECTION_TYPE_FLOAT || type == DT_INTROSPECTION_TYPE_DOUBLE;
}

static double read_float_leaf(const dt_introspection_field_t *field, const void *ptr)
{
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_FLOAT: return (double)*(const float *)ptr;
    case DT_INTROSPECTION_TYPE_DOUBLE: return *(const double *)ptr;
    default: return 0.0;
  }
}

// Evaluates `pred` against `params_blob`, resolving `pred->field` as a one
// segment path off `root` and comparing by enum *name*, never by raw
// integer -- same contract as remote_curve_registry.c's own predicate_holds()
// and remote_vector.c's vector_predicate_holds() (this file keeps its own
// copy rather than sharing a static function across translation units, same
// established idiom as the curve twins).
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
      resolve_error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: vector predicate field '%s' did not resolve"),
        pred->field);
    deliver_error(resolve_error, error);
    return FALSE;
  }
  if(field->header.type != DT_INTROSPECTION_TYPE_ENUM)
  {
    deliver_error(dt_remote_vector_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: vector predicate field '%s' is not an enum"), pred->field),
                  error);
    return FALSE;
  }

  const int value = *(const int *)ptr;
  const char *name = dt_introspection_get_enum_name((dt_introspection_field_t *)field, value);
  if(!name)
  {
    deliver_error(dt_remote_vector_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: vector predicate field '%s' has unknown enum value %d"),
                    pred->field, value),
                  error);
    return FALSE;
  }

  const gboolean matches = !g_strcmp0(name, pred->enum_name);
  *out_holds = (pred->op == DT_REMOTE_PREDICATE_EQ) ? matches : !matches;
  return TRUE;
}

gboolean dt_remote_vector_read_values(const struct dt_iop_module_t *module,
                                      const void *params,
                                      GHashTable **out,
                                      dt_remote_error_t **error)
{
  if(out) *out = NULL;
  if(!module || !params || !out)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to vector value read"));
    return FALSE;
  }

  if(!module->so)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module instance has no .so"));
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), module->op);
    return FALSE;
  }

  GHashTable *result =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)dt_remote_vector_value_free);

  const dt_remote_vector_module_adapter_t *adapter =
    dt_remote_vector_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    *out = result;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_vector_registry_validate(adapter, module->so, &validate_error);
  if(!adapter_valid)
  {
    g_hash_table_destroy(result);
    if(!validate_error)
      validate_error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: vector registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  void *params_blob = (void *)params;
  dt_remote_error_t *read_error = NULL;

  for(guint i = 0; i < adapter->vector_count; i++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[i];

    gboolean active = TRUE;
    if(desc->active_when
       && !predicate_holds(desc->active_when, intro->field, params_blob, &active, &read_error))
      goto fail;

    gboolean writable_now = TRUE;
    if(desc->writable_when
       && !predicate_holds(desc->writable_when, intro->field, params_blob, &writable_now, &read_error))
      goto fail;

    const dt_introspection_field_t *array_field = NULL;
    void *array_ptr = NULL;
    if(!dt_remote_path_resolve(&desc->native, intro->field, params_blob, &array_field, &array_ptr,
                               &read_error))
      goto fail;
    if(array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
       || !is_floating_type(array_field->Array.type))
    {
      read_error = dt_remote_vector_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: vector '%s' native path did not resolve"), desc->name);
      goto fail;
    }

    GArray *values = g_array_sized_new(FALSE, FALSE, sizeof(double), desc->component_count);
    for(guint j = 0; j < desc->component_count; j++)
    {
      dt_introspection_field_t *element_field = NULL;
      void *element_ptr =
        dt_introspection_access_array((dt_introspection_field_t *)array_field, array_ptr, j,
                                      &element_field);
      if(!element_ptr || !element_field || !is_floating_type(element_field->header.type))
      {
        read_error = dt_remote_vector_registry_error_new(
          DT_REMOTE_ERR_INTERNAL, _("internal error: vector '%s' component %u did not resolve"),
          desc->name, j);
        g_array_unref(values);
        goto fail;
      }
      const double value = read_float_leaf(element_field, element_ptr);
      g_array_append_val(values, value);
    }

    dt_remote_vector_value_t *value = g_new0(dt_remote_vector_value_t, 1);
    value->name = g_strdup(desc->name);
    value->values = values;
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
    read_error = dt_remote_vector_registry_error_new(
      DT_REMOTE_ERR_INTERNAL, _("internal error: vector value read failed"));
  deliver_error(read_error, error);
  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
