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

#include "control/remote_vector.h"

#include "common/darktable.h" // _()
#include "develop/imageop.h"

#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_curve.c's own file-local idiom: */
/* remote_edit.c's dt_remote_error_new() is static to that translation    */
/* unit, so every remote_vector* file mirrors it locally rather than       */
/* exporting a shared constructor for one internal error code)             */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_vector_error_new(dt_remote_error_code_t code,
                                                      const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_vector_error_new(dt_remote_error_code_t code,
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

// Builds a DT_REMOTE_ERR_INVALID_VALUE with a details_json object carrying
// "parameter" (always), "constraint" (always, one of the short stable
// strings documented on dt_remote_vector_validate()), and "component_index"
// (only when component_index >= 0) -- same convention as
// dt_remote_curve_validate_error_new() in remote_curve.c.
static dt_remote_error_t *dt_remote_vector_validate_error_new(const char *parameter, int component_index,
                                                               const char *constraint,
                                                               const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *dt_remote_vector_validate_error_new(const char *parameter, int component_index,
                                                               const char *constraint,
                                                               const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = DT_REMOTE_ERR_INVALID_VALUE;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  if(component_index >= 0)
    error->details_json = g_strdup_printf(
      "{\"parameter\":\"%s\",\"component_index\":%d,\"constraint\":\"%s\"}",
      parameter ? parameter : "", component_index, constraint);
  else
    error->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"constraint\":\"%s\"}",
                                          parameter ? parameter : "", constraint);

  return error;
}

static void deliver_vector_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

static dt_remote_error_t *vector_parameter_error_new(dt_remote_error_code_t code,
                                                      const char *parameter,
                                                      const char *constraint,
                                                      const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *vector_parameter_error_new(dt_remote_error_code_t code,
                                                      const char *parameter,
                                                      const char *constraint,
                                                      const char *format, ...)
{
  dt_remote_error_t *error = g_new0(dt_remote_error_t, 1);
  error->code = code;
  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);
  if(parameter && constraint)
    error->details_json = g_strdup_printf("{\"parameter\":\"%s\",\"constraint\":\"%s\"}",
                                          parameter, constraint);
  return error;
}

/* ---------------------------------------------------------------------- */
/* common vector validator                                                 */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_vector_validate(const dt_remote_vector_descriptor_t *desc,
                                   const GArray *values,
                                   dt_remote_error_t **error)
{
  if(!desc)
  {
    if(error)
      *error = dt_remote_vector_error_new(DT_REMOTE_ERR_INTERNAL,
                                          _("internal error: null vector descriptor"));
    return FALSE;
  }

  const char *parameter = desc->name ? desc->name : "";
  const guint n = values ? values->len : 0;

  if(n != desc->component_count)
  {
    if(error)
      *error = dt_remote_vector_validate_error_new(
        parameter, -1, "count_mismatch",
        _("vector '%s' has %u component(s); %u required"), parameter, n, desc->component_count);
    return FALSE;
  }

  for(guint i = 0; i < n; i++)
  {
    const double value = g_array_index(values, double, i);
    const dt_remote_vector_component_t *component = &desc->components[i];
    if(value < component->minimum || value > component->maximum)
    {
      if(error)
        *error = dt_remote_vector_validate_error_new(
          parameter, (int)i, "domain",
          _("vector '%s' component %u has value=%.17g outside domain [%.17g, %.17g]"),
          parameter, i, value, component->minimum, component->maximum);
      return FALSE;
    }
  }

  if(desc->subtype == DT_REMOTE_VECTOR_LEVELS)
  {
    for(guint i = 1; i < n; i++)
    {
      const double previous = g_array_index(values, double, i - 1);
      const double current = g_array_index(values, double, i);
      const double delta = current - previous;

      if(delta <= 0.0)
      {
        if(error)
          *error = dt_remote_vector_validate_error_new(
            parameter, (int)i, "unordered",
            _("vector '%s' components %u and %u are not strictly increasing"), parameter, i - 1, i);
        return FALSE;
      }
      if(delta < desc->minimum_gap)
      {
        if(error)
          *error = dt_remote_vector_validate_error_new(
            parameter, (int)i, "gap",
            _("vector '%s' components %u and %u are %.17g apart; minimum is %.17g"),
            parameter, i - 1, i, delta, desc->minimum_gap);
        return FALSE;
      }
    }
  }

  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* semantic vector mutation engine                                         */
/* ---------------------------------------------------------------------- */

static gboolean vector_predicate_holds(const dt_remote_parameter_predicate_t *predicate,
                                       const dt_introspection_field_t *root,
                                       void *params,
                                       gboolean *out,
                                       dt_remote_error_t **error)
{
  if(!predicate)
  {
    *out = TRUE;
    return TRUE;
  }

  const dt_remote_path_segment_t segment = {
    .type = DT_REMOTE_PATH_FIELD, .value.field = predicate->field,
  };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  if(!dt_remote_path_resolve(&path, root, params, &field, &ptr, error)) return FALSE;
  if(!field || field->header.type != DT_INTROSPECTION_TYPE_ENUM
     || (predicate->op != DT_REMOTE_PREDICATE_EQ && predicate->op != DT_REMOTE_PREDICATE_NE))
  {
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: vector predicate '%s' drifted from introspection"),
                           predicate->field ? predicate->field : ""),
                         error);
    return FALSE;
  }

  const char *live_name = dt_introspection_get_enum_name((dt_introspection_field_t *)field,
                                                          *(const int *)ptr);
  if(!live_name)
  {
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: vector predicate '%s' has an unknown enum value"),
                           predicate->field),
                         error);
    return FALSE;
  }
  const gboolean equal = !g_strcmp0(live_name, predicate->enum_name);
  *out = predicate->op == DT_REMOTE_PREDICATE_EQ ? equal : !equal;
  return TRUE;
}

static const dt_remote_vector_descriptor_t *find_vector_descriptor(
  const dt_remote_vector_module_adapter_t *adapter,
  const char *name)
{
  if(!adapter || !name) return NULL;
  for(guint i = 0; i < adapter->vector_count; i++)
    if(!g_strcmp0(adapter->vectors[i].name, name)) return &adapter->vectors[i];
  return NULL;
}

static gboolean patch_mentions_prepare_field(const dt_remote_vector_module_adapter_t *adapter,
                                             const dt_remote_patch_t *patch)
{
  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *entry = g_ptr_array_index(patch->scalar_values, i);
    for(guint j = 0; entry && j < adapter->prepare_field_count; j++)
      if(!g_strcmp0(entry->name, adapter->prepare_fields[j])) return TRUE;
  }
  return FALSE;
}

static gboolean write_vector_patch(const dt_remote_vector_descriptor_t *desc,
                                   const dt_introspection_field_t *root,
                                   void *params,
                                   const dt_remote_vector_patch_t *vector_patch,
                                   dt_remote_error_t **error)
{
  if(!dt_remote_vector_validate(desc, vector_patch->values, error)) return FALSE;

  const dt_introspection_field_t *array_field = NULL;
  void *array_ptr = NULL;
  if(!dt_remote_path_resolve(&desc->native, root, params, &array_field, &array_ptr, error)) return FALSE;

  if(!array_field || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT)
  {
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: vector '%s' native layout drifted"), desc->name),
                         error);
    return FALSE;
  }

  // Only the leading [0, component_count) elements are ever touched: the
  // preserved tail [component_count, native_capacity) and every other
  // params byte must come out of this call bit-identical to how they went
  // in -- unlike rgbcurve's padding-insignificant convention, a vector's
  // reserved tail may carry meaning this engine does not model.
  for(guint i = 0; i < desc->component_count; i++)
  {
    dt_introspection_field_t *element_field = NULL;
    void *element_ptr = dt_introspection_access_array((dt_introspection_field_t *)array_field,
                                                       array_ptr, i, &element_field);
    if(!element_ptr || !element_field || element_field->header.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      deliver_vector_error(dt_remote_vector_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: vector '%s' component %u drifted"), desc->name, i),
                           error);
      return FALSE;
    }
    const double value = g_array_index(vector_patch->values, double, i);
    *(float *)element_ptr = (float)value;
  }
  return TRUE;
}

gboolean dt_remote_vector_apply_patch(const struct dt_iop_module_t *module,
                                      const void *old_params,
                                      void *new_params,
                                      const dt_remote_patch_t *patch,
                                      dt_remote_error_t **error)
{
  if(!module || !module->so || !old_params || !new_params || !patch)
  {
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: null argument to vector mutation engine")), error);
    return FALSE;
  }

  gboolean has_vector_semantics = FALSE;
  const char *first_vector_name = NULL;
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(semantic && semantic->class_id == DT_REMOTE_PARAMETER_VECTOR)
    {
      has_vector_semantics = TRUE;
      first_vector_name = semantic->value.vector.name;
      break;
    }
  }

  dt_introspection_t *intro = module->so->get_introspection
    ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: module '%s' has no vector introspection"), module->op),
                         error);
    return FALSE;
  }

  const dt_remote_vector_module_adapter_t *adapter =
    dt_remote_vector_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    if(!has_vector_semantics) return TRUE;
    deliver_vector_error(dt_remote_vector_error_new(
                           DT_REMOTE_ERR_UNKNOWN_FIELD,
                           _("unknown semantic vector '%s'"),
                           first_vector_name ? first_vector_name : ""),
                         error);
    return FALSE;
  }

  const gboolean prepare_needed = has_vector_semantics || patch_mentions_prepare_field(adapter, patch);
  // A scalar-only patch unrelated to adapter preparation must remain
  // independent of semantic registry health -- same convention as the
  // curve engine.
  if(!prepare_needed) return TRUE;
  if(!dt_remote_vector_registry_validate(adapter, module->so, error)) return FALSE;

  const dt_remote_vector_context_t context = {
    .module = module, .introspection = intro, .adapter = adapter,
  };

  // Resolve every vector-class request ID before the registry-ordered write
  // loop, skipping every non-vector entry entirely -- the same request may
  // carry curve entries the curve engine handles separately (remote_curve.c).
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(!semantic)
    {
      deliver_vector_error(dt_remote_vector_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: null semantic patch entry")), error);
      return FALSE;
    }
    if(semantic->class_id != DT_REMOTE_PARAMETER_VECTOR) continue;

    const char *name = semantic->value.vector.name;
    if(!find_vector_descriptor(adapter, name))
    {
      deliver_vector_error(dt_remote_vector_error_new(
                             DT_REMOTE_ERR_UNKNOWN_FIELD,
                             _("unknown semantic vector '%s'"), name ? name : ""), error);
      return FALSE;
    }
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_semantic_patch_t *prior = g_ptr_array_index(patch->semantic_values, j);
      if(prior && prior->class_id == DT_REMOTE_PARAMETER_VECTOR
         && !g_strcmp0(prior->value.vector.name, name))
      {
        deliver_vector_error(vector_parameter_error_new(
                               DT_REMOTE_ERR_INVALID_VALUE, name, "duplicate_parameter",
                               _("duplicate semantic vector '%s'"), name), error);
        return FALSE;
      }
    }
  }

  // Apply in descriptor/registry order, never request hash/array order.
  for(guint descriptor_index = 0; descriptor_index < adapter->vector_count; descriptor_index++)
  {
    const dt_remote_vector_descriptor_t *desc = &adapter->vectors[descriptor_index];
    const dt_remote_vector_patch_t *vector_patch = NULL;
    for(guint patch_index = 0;
        patch->semantic_values && patch_index < patch->semantic_values->len;
        patch_index++)
    {
      const dt_remote_semantic_patch_t *semantic =
        g_ptr_array_index(patch->semantic_values, patch_index);
      if(semantic->class_id == DT_REMOTE_PARAMETER_VECTOR
         && !g_strcmp0(semantic->value.vector.name, desc->name))
      {
        vector_patch = &semantic->value.vector;
        break;
      }
    }
    if(!vector_patch) continue;

    gboolean active = TRUE;
    gboolean writable = TRUE;
    if(!vector_predicate_holds(desc->active_when, intro->field, new_params, &active, error)
       || !vector_predicate_holds(desc->writable_when, intro->field, new_params, &writable, error))
      return FALSE;
    if(!active || !writable)
    {
      deliver_vector_error(vector_parameter_error_new(
                             DT_REMOTE_ERR_UNSUPPORTED_FIELD, desc->name,
                             "condition_not_satisfied",
                             _("semantic vector '%s' is inactive or not writable in projected state"),
                             desc->name),
                           error);
      return FALSE;
    }

    if(!write_vector_patch(desc, intro->field, new_params, vector_patch, error)) return FALSE;
  }

  if(prepare_needed && adapter->validate_completed
     && !adapter->validate_completed(&context, new_params, error))
    return FALSE;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
