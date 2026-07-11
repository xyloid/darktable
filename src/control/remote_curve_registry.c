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

// The per-op module adapter registry (curve-classes design doc SS Registry
// lifecycle), the concrete rgbcurve descriptor table (SS Initial registry
// mapping / rgbcurve), and the read-only half of the curve engine API
// (list_schema/read_values -- SS Curve engine API). apply_patch and the
// real prepare/validate_completed callback bodies are Task 8's job; this
// file's rgbcurve adapter instance points both callbacks at stubs that
// unconditionally return TRUE and touch none of their arguments.
//
// Like remote_curve.c, this file never includes JSON, socket, or MCP
// protocol headers -- introspection/GLib (plus develop/imageop.h, for the
// dt_iop_module_so_t/dt_iop_module_t definitions the engine API's public
// signatures only forward-declare in remote_curve.h) only.
//
// Threading: like remote_curve.h/remote_edit.h, resolving a path against a
// live darkroom module's params block must happen from the GTK main
// thread. The static registry tables and the per-adapter validation cache
// below are read-mostly, written once per (adapter) the first time it is
// validated; like the rest of this subsystem, this file assumes single
// (main-context) threaded access and adds no locking of its own.

#include "control/remote_curve.h"

#include "common/darktable.h" // _(), N_()
#include "develop/imageop.h" // dt_iop_module_so_t, dt_iop_module_t

#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_curve.c's own file-local idiom -- see that */
/* file's comment: remote_edit.c's dt_remote_error_new() is static to that */
/* translation unit, so every remote_curve* file mirrors it locally rather */
/* than exporting a shared constructor for one internal error code)        */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_curve_registry_error_new(dt_remote_error_code_t code,
                                                              const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_curve_registry_error_new(dt_remote_error_code_t code,
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

/* ---------------------------------------------------------------------- */
/* rgbcurve descriptor table (curve-classes design doc SS Initial registry */
/* mapping / rgbcurve, and the curve-design-registry-excerpt task brief)   */
/* ---------------------------------------------------------------------- */

// Native layout paths. curve_nodes/curve_num_nodes/curve_type are each
// dt_iop_rgbcurve_node_t/int[DT_IOP_RGBCURVE_MAX_CHANNELS], so every path
// here is the FIELD segment plus one static, compiled-in INDEX segment for
// the channel -- never a request-derived index. Channels 0/0/1/2 for
// master/red/green/blue respectively (master and red share channel 0's
// storage but are distinct semantic IDs -- see the design rationale on
// why two IDs are needed for one storage channel).

static const dt_remote_path_segment_t s_nodes_ch0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t s_nodes_ch0_path = {
  .segments = s_nodes_ch0_segments, .length = G_N_ELEMENTS(s_nodes_ch0_segments)
};

static const dt_remote_path_segment_t s_nodes_ch1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_introspection_path_t s_nodes_ch1_path = {
  .segments = s_nodes_ch1_segments, .length = G_N_ELEMENTS(s_nodes_ch1_segments)
};

static const dt_remote_path_segment_t s_nodes_ch2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_introspection_path_t s_nodes_ch2_path = {
  .segments = s_nodes_ch2_segments, .length = G_N_ELEMENTS(s_nodes_ch2_segments)
};

static const dt_remote_path_segment_t s_count_ch0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_num_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t s_count_ch0_path = {
  .segments = s_count_ch0_segments, .length = G_N_ELEMENTS(s_count_ch0_segments)
};

static const dt_remote_path_segment_t s_count_ch1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_num_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_introspection_path_t s_count_ch1_path = {
  .segments = s_count_ch1_segments, .length = G_N_ELEMENTS(s_count_ch1_segments)
};

static const dt_remote_path_segment_t s_count_ch2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_num_nodes" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_introspection_path_t s_count_ch2_path = {
  .segments = s_count_ch2_segments, .length = G_N_ELEMENTS(s_count_ch2_segments)
};

static const dt_remote_path_segment_t s_type_ch0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_type" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_introspection_path_t s_type_ch0_path = {
  .segments = s_type_ch0_segments, .length = G_N_ELEMENTS(s_type_ch0_segments)
};

static const dt_remote_path_segment_t s_type_ch1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_type" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_introspection_path_t s_type_ch1_path = {
  .segments = s_type_ch1_segments, .length = G_N_ELEMENTS(s_type_ch1_segments)
};

static const dt_remote_path_segment_t s_type_ch2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "curve_type" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_introspection_path_t s_type_ch2_path = {
  .segments = s_type_ch2_segments, .length = G_N_ELEMENTS(s_type_ch2_segments)
};

// rgbcurve has no internal version leaf: zero-length path, per
// dt_remote_native_curve_layout_t's documented convention.
static const dt_remote_introspection_path_t s_no_internal_version_path = {
  .segments = NULL, .length = 0
};

// Predicates. Both compare dt_iop_rgbcurve_params_t::curve_autoscale
// against the stable enum name generated for DT_S_SCALE_MANUAL_RGB
// (src/iop/rgbcurve.c). Per tools/introspection/ast.pm
// ast_type_node::get_introspection_code() (`"$id", $id, $description` --
// the enum tuple's string name is exactly the C enumerator token, never a
// derived/mangled form), that name is the literal token
// "DT_S_SCALE_MANUAL_RGB"; test_registry_resolves_manual_rgb_enum_name in
// test_remote_curve_registry.c confirms this against the real loaded
// rgbcurve module's introspection tree rather than trusting this comment
// alone.
static const dt_remote_parameter_predicate_t s_master_predicate = {
  .field = "curve_autoscale", .op = DT_REMOTE_PREDICATE_NE, .enum_name = "DT_S_SCALE_MANUAL_RGB"
};
static const dt_remote_parameter_predicate_t s_manual_predicate = {
  .field = "curve_autoscale", .op = DT_REMOTE_PREDICATE_EQ, .enum_name = "DT_S_SCALE_MANUAL_RGB"
};

#define RGBCURVE_INTERPOLATION_MASK \
  ((1u << DT_REMOTE_CURVE_CUBIC_SPLINE) | (1u << DT_REMOTE_CURVE_CATMULL_ROM) \
   | (1u << DT_REMOTE_CURVE_MONOTONE_HERMITE))

static const dt_remote_curve_descriptor_t s_rgbcurve_curves[] = {
  {
    .name = "curve.master",
    .display_name_msgid = N_("master"),
    .description_msgid = NULL,
    .native = { .nodes = s_nodes_ch0_path,
               .count = s_count_ch0_path,
               .type = s_type_ch0_path,
               .x_field = "x",
               .y_field = "y",
               .internal_version = s_no_internal_version_path,
               .internal_version_value = 0 },
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .minimum_points = 2,
    .maximum_points = 20,
    .minimum_x_spacing = 0.0025,
    .adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN,
    .minimum_wrap_spacing = 0.0,
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
    .strict_x_order = TRUE,
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,
    .active_when = &s_master_predicate,
    .writable_when = &s_master_predicate,
    .periodic_when = NULL,
  },
  {
    .name = "curve.red",
    .display_name_msgid = N_("R"),
    .description_msgid = NULL,
    .native = { .nodes = s_nodes_ch0_path,
               .count = s_count_ch0_path,
               .type = s_type_ch0_path,
               .x_field = "x",
               .y_field = "y",
               .internal_version = s_no_internal_version_path,
               .internal_version_value = 0 },
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .minimum_points = 2,
    .maximum_points = 20,
    .minimum_x_spacing = 0.0025,
    .adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN,
    .minimum_wrap_spacing = 0.0,
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
    .strict_x_order = TRUE,
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,
    .active_when = &s_manual_predicate,
    .writable_when = &s_manual_predicate,
    .periodic_when = NULL,
  },
  {
    .name = "curve.green",
    .display_name_msgid = N_("G"),
    .description_msgid = NULL,
    .native = { .nodes = s_nodes_ch1_path,
               .count = s_count_ch1_path,
               .type = s_type_ch1_path,
               .x_field = "x",
               .y_field = "y",
               .internal_version = s_no_internal_version_path,
               .internal_version_value = 0 },
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .minimum_points = 2,
    .maximum_points = 20,
    .minimum_x_spacing = 0.0025,
    .adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN,
    .minimum_wrap_spacing = 0.0,
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
    .strict_x_order = TRUE,
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,
    .active_when = &s_manual_predicate,
    .writable_when = &s_manual_predicate,
    .periodic_when = NULL,
  },
  {
    .name = "curve.blue",
    .display_name_msgid = N_("B"),
    .description_msgid = NULL,
    .native = { .nodes = s_nodes_ch2_path,
               .count = s_count_ch2_path,
               .type = s_type_ch2_path,
               .x_field = "x",
               .y_field = "y",
               .internal_version = s_no_internal_version_path,
               .internal_version_value = 0 },
    .x = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .y = { .minimum = 0.0, .maximum = 1.0, .unit = "normalized" },
    .minimum_points = 2,
    .maximum_points = 20,
    .minimum_x_spacing = 0.0025,
    .adjacent_spacing_rule = DT_REMOTE_SPACING_GREATER_THAN,
    .minimum_wrap_spacing = 0.0,
    .wrap_spacing_rule = DT_REMOTE_SPACING_NONE,
    .strict_x_order = TRUE,
    .boundary_point_policy = DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL,
    .interpolation_mask = RGBCURVE_INTERPOLATION_MASK,
    .default_interpolation = DT_REMOTE_CURVE_MONOTONE_HERMITE,
    .active_when = &s_manual_predicate,
    .writable_when = &s_manual_predicate,
    .periodic_when = NULL,
  },
};

// Task 8 fills these in with the real mode-transition logic; Task 6 wires
// stubs that touch none of their arguments and unconditionally succeed, so
// that dt_remote_curve_module_adapter_t is a fully-formed, callable (if
// inert) instance rather than a struct with dangling function pointers.
static gboolean rgbcurve_prepare_stub(const struct dt_remote_curve_context_t *ctx,
                                      const void *old_params,
                                      void *new_params,
                                      const dt_remote_patch_t *patch,
                                      dt_remote_error_t **error)
{
  (void)ctx;
  (void)old_params;
  (void)new_params;
  (void)patch;
  (void)error;
  return TRUE;
}

static gboolean rgbcurve_validate_completed_stub(const struct dt_remote_curve_context_t *ctx,
                                                 const void *new_params,
                                                 dt_remote_error_t **error)
{
  (void)ctx;
  (void)new_params;
  (void)error;
  return TRUE;
}

static const char *const s_rgbcurve_prepare_fields[] = { "curve_autoscale", "compensate_middle_grey" };

static const dt_remote_curve_module_adapter_t s_rgbcurve_adapter = {
  .operation = "rgbcurve",
  .minimum_params_version = 1,
  .maximum_params_version = 1,
  .curves = s_rgbcurve_curves,
  .curve_count = G_N_ELEMENTS(s_rgbcurve_curves),
  .prepare_fields = s_rgbcurve_prepare_fields,
  .prepare_field_count = G_N_ELEMENTS(s_rgbcurve_prepare_fields),
  .prepare = rgbcurve_prepare_stub,
  .validate_completed = rgbcurve_validate_completed_stub,
};

// The full adapter table. Only rgbcurve for now; a future op (e.g.
// tonecurve) adds another entry here, not a parallel lookup mechanism.
static const dt_remote_curve_module_adapter_t *const s_adapters[] = {
  &s_rgbcurve_adapter,
};

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

const dt_remote_curve_module_adapter_t *
dt_remote_curve_registry_lookup(const char *operation, guint params_version)
{
  if(!operation) return NULL;

  for(guint i = 0; i < G_N_ELEMENTS(s_adapters); i++)
  {
    const dt_remote_curve_module_adapter_t *adapter = s_adapters[i];
    if(!g_strcmp0(adapter->operation, operation)
       && params_version >= adapter->minimum_params_version
       && params_version <= adapter->maximum_params_version)
      return adapter;
  }

  return NULL;
}

// Process-lifetime validation cache, keyed by adapter pointer identity
// (every adapter instance is static process-lifetime data, so pointer
// identity is a valid, stable cache key -- no string hashing needed).
typedef struct dt_remote_curve_registry_cache_entry_t
{
  gboolean validated;       // TRUE once this adapter has been shape-checked
  gboolean adapter_valid;   // TRUE iff every descriptor's shape was valid
  gboolean *descriptor_valid; // owned; adapter->curve_count entries
} dt_remote_curve_registry_cache_entry_t;

static GHashTable *s_validation_cache = NULL; // adapter ptr -> cache entry ptr; never freed
                                              // (process lifetime, like the
                                              // static descriptors/adapters
                                              // it caches results for)

static dt_remote_curve_registry_cache_entry_t *
get_cache_entry(const dt_remote_curve_module_adapter_t *adapter)
{
  if(!s_validation_cache)
    s_validation_cache = g_hash_table_new(g_direct_hash, g_direct_equal);

  dt_remote_curve_registry_cache_entry_t *entry = g_hash_table_lookup(s_validation_cache, adapter);
  if(!entry)
  {
    entry = g_new0(dt_remote_curve_registry_cache_entry_t, 1);
    g_hash_table_insert(s_validation_cache, (gpointer)adapter, entry);
  }
  return entry;
}

static gboolean is_integer_or_enum_type(dt_introspection_type_t type)
{
  switch(type)
  {
    case DT_INTROSPECTION_TYPE_CHAR:
    case DT_INTROSPECTION_TYPE_INT8:
    case DT_INTROSPECTION_TYPE_UINT8:
    case DT_INTROSPECTION_TYPE_SHORT:
    case DT_INTROSPECTION_TYPE_USHORT:
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_LONG:
    case DT_INTROSPECTION_TYPE_ULONG:
    case DT_INTROSPECTION_TYPE_ENUM:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean is_floating_type(dt_introspection_type_t type)
{
  return type == DT_INTROSPECTION_TYPE_FLOAT || type == DT_INTROSPECTION_TYPE_DOUBLE;
}

// Checks one descriptor's native layout against `root` (the module's whole
// params struct field, DT_INTROSPECTION_TYPE_STRUCT) using the Task 4
// cursor. `dummy_blob` is a zero-sized/dummy blob pointer purely to
// satisfy dt_remote_path_resolve()'s signature -- only field-shape output
// is consulted here, never the resolved pointer's pointee.
static gboolean descriptor_shape_is_valid(const dt_remote_curve_descriptor_t *desc,
                                          const dt_introspection_field_t *root,
                                          void *dummy_blob)
{
  const dt_introspection_field_t *nodes_field = NULL;
  if(!dt_remote_path_resolve(&desc->native.nodes, root, dummy_blob, &nodes_field, NULL, NULL))
    return FALSE;
  if(nodes_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || !nodes_field->Array.field
     || nodes_field->Array.field->header.type != DT_INTROSPECTION_TYPE_STRUCT)
    return FALSE;

  const dt_introspection_field_t *count_field = NULL;
  if(!dt_remote_path_resolve(&desc->native.count, root, dummy_blob, &count_field, NULL, NULL))
    return FALSE;
  if(!is_integer_or_enum_type(count_field->header.type))
    return FALSE;

  const dt_introspection_field_t *type_field = NULL;
  if(!dt_remote_path_resolve(&desc->native.type, root, dummy_blob, &type_field, NULL, NULL))
    return FALSE;
  if(!is_integer_or_enum_type(type_field->header.type))
    return FALSE;

  dt_introspection_field_t *node_struct = nodes_field->Array.field;
  dt_introspection_field_t *x_field = NULL;
  dt_introspection_field_t *y_field = NULL;
  void *x_ptr = dt_introspection_get_child(node_struct, dummy_blob, desc->native.x_field, &x_field);
  void *y_ptr = dt_introspection_get_child(node_struct, dummy_blob, desc->native.y_field, &y_field);
  if(!x_ptr || !x_field || !is_floating_type(x_field->header.type)) return FALSE;
  if(!y_ptr || !y_field || !is_floating_type(y_field->header.type)) return FALSE;

  return TRUE;
}

gboolean dt_remote_curve_registry_validate(const dt_remote_curve_module_adapter_t *adapter,
                                           const dt_introspection_t *introspection,
                                           dt_remote_error_t **error)
{
  if(!adapter || !introspection || !introspection->field)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: null adapter or introspection passed to curve registry validation"));
    return FALSE;
  }

  dt_remote_curve_registry_cache_entry_t *cache = get_cache_entry(adapter);
  if(cache->validated)
  {
    if(!cache->adapter_valid && error)
      *error = dt_remote_curve_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("curve adapter '%s' has one or more descriptors whose native layout does not match introspection"),
        adapter->operation ? adapter->operation : "");
    return cache->adapter_valid;
  }

  cache->descriptor_valid = g_new0(gboolean, adapter->curve_count);

  guint8 dummy_byte = 0;
  gboolean all_valid = TRUE;
  const char *first_failure = NULL;

  // Every descriptor is checked -- a shape mismatch on one never stops the
  // rest of the adapter's descriptors from being checked (and cached)
  // too; see the header comment on this function.
  for(guint i = 0; i < adapter->curve_count; i++)
  {
    const dt_remote_curve_descriptor_t *desc = &adapter->curves[i];
    const gboolean ok = descriptor_shape_is_valid(desc, introspection->field, &dummy_byte);
    cache->descriptor_valid[i] = ok;
    if(!ok)
    {
      all_valid = FALSE;
      if(!first_failure) first_failure = desc->name;
    }
  }

  cache->validated = TRUE;
  cache->adapter_valid = all_valid;

  if(!all_valid)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("curve descriptor '%s' native layout does not match introspection"),
        first_failure ? first_failure : "");
    return FALSE;
  }
  return TRUE;
}

// Whether descriptor `index` on `adapter` passed its shape check.
// dt_remote_curve_registry_validate() must already have been run for
// `adapter` (list_schema/read_values below always do this first); if it
// has not, this conservatively reports "invalid" rather than serving an
// unvalidated descriptor.
static gboolean descriptor_is_valid_cached(const dt_remote_curve_module_adapter_t *adapter, guint index)
{
  dt_remote_curve_registry_cache_entry_t *cache = get_cache_entry(adapter);
  if(!cache->validated || !cache->descriptor_valid) return FALSE;
  return cache->descriptor_valid[index];
}

/* ---------------------------------------------------------------------- */
/* curve engine API -- read-only half (list_schema/read_values)            */
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

gboolean dt_remote_curve_list_schema(const struct dt_iop_module_so_t *module_so,
                                     GPtrArray **out,
                                     dt_remote_error_t **error)
{
  if(!module_so || !out)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(DT_REMOTE_ERR_INTERNAL,
                                                  _("internal error: null argument to curve schema listing"));
    return FALSE;
  }

  dt_introspection_t *intro = module_so->get_introspection ? module_so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(DT_REMOTE_ERR_INTERNAL,
                                                  _("internal error: module '%s' has no introspection"),
                                                  module_so->op);
    return FALSE;
  }

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_curve_schema_free);

  const dt_remote_curve_module_adapter_t *adapter =
    dt_remote_curve_registry_lookup(module_so->op, (guint)intro->params_version);
  if(!adapter)
  {
    // No adapter for this op/version: no semantic curve advertised, not an
    // error (native array fields simply remain unsupported elsewhere).
    *out = result;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  dt_remote_curve_registry_validate(adapter, intro, &validate_error);
  if(validate_error) dt_remote_error_free(validate_error); // per-descriptor cache below is authoritative

  for(guint i = 0; i < adapter->curve_count; i++)
  {
    if(!descriptor_is_valid_cached(adapter, i)) continue;

    const dt_remote_curve_descriptor_t *desc = &adapter->curves[i];
    dt_remote_curve_schema_t *schema = g_new0(dt_remote_curve_schema_t, 1);

    schema->name = g_strdup(desc->name);
    schema->display_name = g_strdup(desc->display_name_msgid ? _(desc->display_name_msgid) : "");
    schema->description = desc->description_msgid ? g_strdup(_(desc->description_msgid)) : NULL;
    schema->x.minimum = desc->x.minimum;
    schema->x.maximum = desc->x.maximum;
    schema->x.unit = g_strdup(desc->x.unit);
    schema->y.minimum = desc->y.minimum;
    schema->y.maximum = desc->y.maximum;
    schema->y.unit = g_strdup(desc->y.unit);
    schema->minimum_points = desc->minimum_points;
    schema->maximum_points = desc->maximum_points;
    schema->minimum_x_spacing = desc->minimum_x_spacing;
    schema->adjacent_spacing_rule = desc->adjacent_spacing_rule;
    schema->minimum_wrap_spacing = desc->minimum_wrap_spacing;
    schema->wrap_spacing_rule = desc->wrap_spacing_rule;
    schema->strict_x_order = desc->strict_x_order;
    // Unconditionally periodic only when there is no periodic_when gate
    // and the descriptor declares wrap spacing at all; rgbcurve's four
    // descriptors have neither, so this is FALSE for all of them.
    schema->periodic_x = (!desc->periodic_when && desc->wrap_spacing_rule != DT_REMOTE_SPACING_NONE);
    schema->boundary_point_policy = desc->boundary_point_policy;
    schema->interpolation_mask = desc->interpolation_mask;
    schema->default_interpolation = desc->default_interpolation;
    schema->writability = desc->writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NEVER;
    schema->active_when = condition_from_predicate(desc->active_when);
    schema->writable_when = condition_from_predicate(desc->writable_when);
    schema->periodic_when = condition_from_predicate(desc->periodic_when);

    g_ptr_array_add(result, schema);
  }

  *out = result;
  return TRUE;
}

// Reads the int stored at a resolved integer/enum leaf (whatever its
// native width) as a gint64, without re-adding the leaf's offset (`ptr`
// already points directly at it -- see dt_remote_path_resolve()'s
// contract). Mirrors dt_remote_value_from_field()'s int-family switch in
// remote_edit.c, but reads through an already-offset pointer instead of
// re-deriving one from params_blob + f->header.offset.
static gint64 read_integer_leaf(const dt_introspection_field_t *field, const void *ptr)
{
  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_CHAR: return *(const char *)ptr;
    case DT_INTROSPECTION_TYPE_INT8: return *(const int8_t *)ptr;
    case DT_INTROSPECTION_TYPE_UINT8: return *(const uint8_t *)ptr;
    case DT_INTROSPECTION_TYPE_SHORT: return *(const short *)ptr;
    case DT_INTROSPECTION_TYPE_USHORT: return *(const unsigned short *)ptr;
    case DT_INTROSPECTION_TYPE_INT: return *(const int *)ptr;
    case DT_INTROSPECTION_TYPE_UINT: return *(const unsigned int *)ptr;
    case DT_INTROSPECTION_TYPE_LONG: return *(const long *)ptr;
    case DT_INTROSPECTION_TYPE_ULONG: return (gint64)*(const unsigned long *)ptr;
    case DT_INTROSPECTION_TYPE_ENUM: return *(const int *)ptr;
    default: return 0;
  }
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

// Maps a native curve_type int to dt_remote_curve_interpolation_t by
// explicit value identity (never cast/reinterpret), per
// src/common/curve_tools.h's CUBIC_SPLINE=0/CATMULL_ROM=1/
// MONOTONE_HERMITE=2 -- numerically identical to
// dt_remote_curve_interpolation_t's own values today, but mapped
// explicitly so registry drift in one enum can never silently corrupt the
// other.
static gboolean map_native_interpolation(gint64 native_value, dt_remote_curve_interpolation_t *out)
{
  switch(native_value)
  {
    case 0: *out = DT_REMOTE_CURVE_CUBIC_SPLINE; return TRUE;
    case 1: *out = DT_REMOTE_CURVE_CATMULL_ROM; return TRUE;
    case 2: *out = DT_REMOTE_CURVE_MONOTONE_HERMITE; return TRUE;
    default: return FALSE;
  }
}

// Evaluates `pred` (a single primitive-enum-field-vs-stable-enum-name
// predicate) against `params_blob`, resolving `pred->field` as a one
// segment path off `root` and comparing by enum *name*, never by raw
// integer. Returns FALSE (leaving `*out_holds` untouched) only on
// registry/introspection drift (unresolvable field, or a field that turns
// out not to be an enum) -- callers treat that the same as "does not
// hold" for active/writable purposes, matching the read_values contract.
static gboolean predicate_holds(const dt_remote_parameter_predicate_t *pred,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                gboolean *out_holds)
{
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = pred->field };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };

  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!dt_remote_path_resolve(&path, root, params_blob, &field, &ptr, NULL)) return FALSE;
  if(field->header.type != DT_INTROSPECTION_TYPE_ENUM) return FALSE;

  const int value = *(const int *)ptr;
  const char *name = dt_introspection_get_enum_name((dt_introspection_field_t *)field, value);
  const gboolean matches = name && !g_strcmp0(name, pred->enum_name);
  *out_holds = (pred->op == DT_REMOTE_PREDICATE_EQ) ? matches : !matches;
  return TRUE;
}

gboolean dt_remote_curve_read_values(const struct dt_iop_module_t *module,
                                     const void *params,
                                     GHashTable **out,
                                     dt_remote_error_t **error)
{
  if(!module || !params || !out)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(DT_REMOTE_ERR_INTERNAL,
                                                  _("internal error: null argument to curve value read"));
    return FALSE;
  }

  if(!module->so)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(DT_REMOTE_ERR_INTERNAL,
                                                  _("internal error: module instance has no .so"));
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_curve_registry_error_new(DT_REMOTE_ERR_INTERNAL,
                                                  _("internal error: module '%s' has no introspection"),
                                                  module->op);
    return FALSE;
  }

  GHashTable *result =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)dt_remote_curve_value_free);

  const dt_remote_curve_module_adapter_t *adapter =
    dt_remote_curve_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    *out = result;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  dt_remote_curve_registry_validate(adapter, intro, &validate_error);
  if(validate_error) dt_remote_error_free(validate_error); // per-descriptor cache below is authoritative

  void *params_blob = (void *)params;

  for(guint i = 0; i < adapter->curve_count; i++)
  {
    if(!descriptor_is_valid_cached(adapter, i)) continue;

    const dt_remote_curve_descriptor_t *desc = &adapter->curves[i];

    gboolean active = TRUE;
    if(desc->active_when && !predicate_holds(desc->active_when, intro->field, params_blob, &active))
      active = FALSE;

    gboolean writable_now = FALSE;
    if(desc->writable_when && !predicate_holds(desc->writable_when, intro->field, params_blob, &writable_now))
      writable_now = FALSE;

    const dt_introspection_field_t *nodes_field = NULL;
    void *nodes_ptr = NULL;
    const dt_introspection_field_t *count_field = NULL;
    void *count_ptr = NULL;
    const dt_introspection_field_t *type_field = NULL;
    void *type_ptr = NULL;

    const gboolean resolved =
      dt_remote_path_resolve(&desc->native.nodes, intro->field, params_blob, &nodes_field, &nodes_ptr, NULL)
      && dt_remote_path_resolve(&desc->native.count, intro->field, params_blob, &count_field, &count_ptr, NULL)
      && dt_remote_path_resolve(&desc->native.type, intro->field, params_blob, &type_field, &type_ptr, NULL);
    if(!resolved) continue; // shape already validated above; defensive only

    const gint64 raw_count = read_integer_leaf(count_field, count_ptr);
    const guint capacity = (guint)nodes_field->Array.count;
    guint n = (raw_count < 0) ? 0 : (guint)raw_count;
    if(n > capacity) n = capacity;

    GArray *points = g_array_sized_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t), n);
    for(guint j = 0; j < n; j++)
    {
      dt_introspection_field_t *node_struct_field = NULL;
      void *node_ptr =
        dt_introspection_access_array((dt_introspection_field_t *)nodes_field, nodes_ptr, j, &node_struct_field);
      if(!node_ptr) break;

      dt_introspection_field_t *x_field = NULL;
      dt_introspection_field_t *y_field = NULL;
      void *x_ptr = dt_introspection_get_child(node_struct_field, node_ptr, desc->native.x_field, &x_field);
      void *y_ptr = dt_introspection_get_child(node_struct_field, node_ptr, desc->native.y_field, &y_field);
      if(!x_ptr || !y_ptr) break;

      const dt_remote_curve_point_t point = { read_float_leaf(x_field, x_ptr), read_float_leaf(y_field, y_ptr) };
      g_array_append_val(points, point);
    }

    const gint64 raw_type = read_integer_leaf(type_field, type_ptr);
    dt_remote_curve_interpolation_t interpolation = desc->default_interpolation;
    if(!map_native_interpolation(raw_type, &interpolation))
    {
      // registry/introspection drift: the live curve_type value is none of
      // the known native interpolation IDs. Fail closed rather than
      // silently substituting the descriptor default -- same convention as
      // the other internal errors in this function.
      g_array_unref(points);
      g_hash_table_destroy(result);
      if(error)
        *error = dt_remote_curve_registry_error_new(
          DT_REMOTE_ERR_INTERNAL,
          _("internal error: curve '%s' has out-of-range native curve_type %" G_GINT64_FORMAT),
          desc->name, raw_type);
      return FALSE;
    }

    dt_remote_curve_value_t *value = g_new0(dt_remote_curve_value_t, 1);
    value->name = g_strdup(desc->name);
    value->points = points;
    value->interpolation = interpolation;
    value->active = active;
    value->effective = active;
    value->writable_now = writable_now;
    value->periodic_x = (!desc->periodic_when && desc->wrap_spacing_rule != DT_REMOTE_SPACING_NONE);

    g_hash_table_insert(result, g_strdup(desc->name), value);
  }

  *out = result;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
