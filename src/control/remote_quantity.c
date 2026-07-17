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

#include "control/remote_quantity.h"

#include "common/darktable.h" // _()
#include "develop/imageop.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

// Defined in remote_quantity_registry.c, not part of the public header:
// shares the hooks-override storage between the read half (that file's
// dt_remote_quantity_read_values()) and the write half (this file's
// dt_remote_quantity_apply_entries()) -- same "static data + seams" file
// split the class's own top comment documents. Not thread-safe, same
// caveat as the override setter itself (remote_quantity.h).
dt_remote_quantity_write_hook_t dt_remote_quantity_registry_current_write_hook(void);

/* ---------------------------------------------------------------------- */
/* error helpers (mirrors remote_band.c's own file-local idiom: see that   */
/* file's comment)                                                         */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_quantity_error_new(dt_remote_error_code_t code,
                                                        const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_quantity_error_new(dt_remote_error_code_t code,
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
// "parameter" (always), "component" (always -- every dt_remote_quantity_validate()
// rejection is attributable to exactly one named component), and
// "constraint" (always, one of the short stable strings documented on
// dt_remote_quantity_validate()) -- same convention as
// dt_remote_band_validate_error_new() in remote_band.c, with "component"
// standing in for that function's "array"/"index" pair since a quantity's
// components are named, not positional.
static dt_remote_error_t *dt_remote_quantity_validate_error_new(const char *parameter,
                                                                 const char *component,
                                                                 const char *constraint,
                                                                 const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *dt_remote_quantity_validate_error_new(const char *parameter,
                                                                 const char *component,
                                                                 const char *constraint,
                                                                 const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = DT_REMOTE_ERR_INVALID_VALUE;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  error->details_json = g_strdup_printf(
    "{\"parameter\":\"%s\",\"component\":\"%s\",\"constraint\":\"%s\"}",
    parameter ? parameter : "", component ? component : "", constraint);

  return error;
}

static void deliver_quantity_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

// details_json {"parameter", "constraint"} only -- used for apply_entries-
// level rejections that are not attributable to a single component
// (unknown ID, duplicate ID, native conflict, condition not satisfied).
// Same convention as remote_band.c's band_parameter_error_new().
static dt_remote_error_t *quantity_parameter_error_new(dt_remote_error_code_t code,
                                                        const char *parameter,
                                                        const char *constraint,
                                                        const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *quantity_parameter_error_new(dt_remote_error_code_t code,
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
/* common quantity validator                                               */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_quantity_validate(const dt_remote_quantity_descriptor_t *desc,
                                     const GPtrArray *values,
                                     dt_remote_error_t **error)
{
  if(!desc)
  {
    if(error)
      *error = dt_remote_quantity_error_new(DT_REMOTE_ERR_INTERNAL,
                                            _("internal error: null quantity descriptor"));
    return FALSE;
  }

  const char *parameter = desc->name ? desc->name : "";
  const guint n = values ? values->len : 0;

  // Duplicate component names within `values`.
  for(guint i = 0; i < n; i++)
  {
    const dt_remote_quantity_component_value_t *item = g_ptr_array_index(values, i);
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_quantity_component_value_t *prior = g_ptr_array_index(values, j);
      if(!g_strcmp0(item->name, prior->name))
      {
        if(error)
          *error = dt_remote_quantity_validate_error_new(
            parameter, item->name, "duplicate_component",
            _("quantity '%s' has a duplicate component '%s'"), parameter,
            item->name ? item->name : "");
        return FALSE;
      }
    }
  }

  // Unknown members: every name in `values` must be one of `desc`'s
  // component names.
  for(guint i = 0; i < n; i++)
  {
    const dt_remote_quantity_component_value_t *item = g_ptr_array_index(values, i);
    gboolean known = FALSE;
    for(guint c = 0; c < desc->component_count; c++)
      if(!g_strcmp0(desc->components[c].name, item->name)) { known = TRUE; break; }
    if(!known)
    {
      if(error)
        *error = dt_remote_quantity_validate_error_new(
          parameter, item->name, "unknown_component",
          _("quantity '%s' has unknown component '%s'"), parameter, item->name ? item->name : "");
      return FALSE;
    }
  }

  // Missing members: every `desc` component name must appear in `values`.
  for(guint c = 0; c < desc->component_count; c++)
  {
    const char *component_name = desc->components[c].name;
    gboolean found = FALSE;
    for(guint i = 0; i < n; i++)
    {
      const dt_remote_quantity_component_value_t *item = g_ptr_array_index(values, i);
      if(!g_strcmp0(item->name, component_name)) { found = TRUE; break; }
    }
    if(!found)
    {
      if(error)
        *error = dt_remote_quantity_validate_error_new(
          parameter, component_name, "missing_component",
          _("quantity '%s' is missing component '%s'"), parameter, component_name ? component_name : "");
      return FALSE;
    }
  }

  // Finiteness + domain, in descriptor order. The name-set checks above
  // guarantee every component below has exactly one matching entry in
  // `values`.
  for(guint c = 0; c < desc->component_count; c++)
  {
    const dt_remote_quantity_component_descriptor_t *component = &desc->components[c];
    const dt_remote_quantity_component_value_t *item = NULL;
    for(guint i = 0; i < n; i++)
    {
      const dt_remote_quantity_component_value_t *candidate = g_ptr_array_index(values, i);
      if(!g_strcmp0(candidate->name, component->name)) { item = candidate; break; }
    }
    g_assert(item);

    if(!isfinite(item->value))
    {
      if(error)
        *error = dt_remote_quantity_validate_error_new(
          parameter, component->name, "non_finite",
          _("quantity '%s' component '%s' has a non-finite value"), parameter, component->name);
      return FALSE;
    }
    if(item->value < component->minimum || item->value > component->maximum)
    {
      if(error)
        *error = dt_remote_quantity_validate_error_new(
          parameter, component->name, "domain",
          _("quantity '%s' component '%s' has value=%.17g outside domain [%.17g, %.17g]"),
          parameter, component->name, item->value, component->minimum, component->maximum);
      return FALSE;
    }
  }

  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* semantic quantity mutation engine                                       */
/* ---------------------------------------------------------------------- */

static gboolean quantity_predicate_holds(const dt_remote_parameter_predicate_t *predicate,
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
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: quantity predicate '%s' drifted from introspection"),
                             predicate->field ? predicate->field : ""),
                           error);
    return FALSE;
  }

  const char *live_name = dt_introspection_get_enum_name((dt_introspection_field_t *)field,
                                                          *(const int *)ptr);
  if(!live_name)
  {
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: quantity predicate '%s' has an unknown enum value"),
                             predicate->field),
                           error);
    return FALSE;
  }
  const gboolean equal = !g_strcmp0(live_name, predicate->enum_name);
  *out = predicate->op == DT_REMOTE_PREDICATE_EQ ? equal : !equal;
  return TRUE;
}

static const dt_remote_quantity_descriptor_t *find_quantity_descriptor(
  const dt_remote_quantity_module_adapter_t *adapter,
  const char *name)
{
  if(!adapter || !name) return NULL;
  for(guint i = 0; i < adapter->quantity_count; i++)
    if(!g_strcmp0(adapter->quantities[i].name, name)) return &adapter->quantities[i];
  return NULL;
}

static const dt_remote_quantity_patch_t *find_quantity_entry(GPtrArray *entries, const char *name)
{
  if(!entries || !name) return NULL;
  for(guint i = 0; i < entries->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(entries, i);
    if(semantic && !g_strcmp0(semantic->value.quantity.name, name))
      return &semantic->value.quantity;
  }
  return NULL;
}

// One (offset, size) byte range within the params blob that a resolved
// `native_fields` name occupies -- resolved once per apply_entries() call
// (native_fields is adapter-level, not per-descriptor, and the layout
// cannot change mid-call), then reused by the post-write byte-check for
// every descriptor's write.
typedef struct native_field_range_t
{
  size_t offset;
  size_t size;
} native_field_range_t;

// Resolves every one of `adapter->native_fields` as a single-field path
// against `root`/`params_blob` and records its byte range. Every failure
// here is registry/introspection drift (dt_remote_quantity_registry_validate()
// already proved these names resolve to writable scalar float leaves), never
// caller error.
static gboolean resolve_native_field_ranges(const dt_remote_quantity_module_adapter_t *adapter,
                                            const dt_introspection_field_t *root,
                                            void *params_blob,
                                            native_field_range_t *out_ranges,
                                            dt_remote_error_t **error)
{
  for(guint i = 0; i < adapter->native_field_count; i++)
  {
    const dt_remote_path_segment_t segment = {
      .type = DT_REMOTE_PATH_FIELD, .value.field = adapter->native_fields[i],
    };
    const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };
    const dt_introspection_field_t *field = NULL;
    void *ptr = NULL;
    if(!dt_remote_path_resolve(&path, root, params_blob, &field, &ptr, error)) return FALSE;
    if(!field || field->header.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      deliver_quantity_error(dt_remote_quantity_error_new(
                               DT_REMOTE_ERR_INTERNAL,
                               _("internal error: quantity native field '%s' layout drifted"),
                               adapter->native_fields[i]),
                             error);
      return FALSE;
    }
    out_ranges[i].offset = (size_t)((const guint8 *)ptr - (const guint8 *)params_blob);
    out_ranges[i].size = field->header.size;
  }
  return TRUE;
}

static gboolean byte_offset_is_native(const native_field_range_t *ranges, guint count, size_t offset)
{
  for(guint i = 0; i < count; i++)
    if(offset >= ranges[i].offset && offset < ranges[i].offset + ranges[i].size)
      return TRUE;
  return FALSE;
}

// TRUE iff every byte that differs between `before` and `after` (both
// `size` bytes) falls within one of `ranges` -- the byte-check the design
// doc requires after every conversion write hook call.
static gboolean only_native_bytes_changed(const guint8 *before, const guint8 *after, size_t size,
                                          const native_field_range_t *ranges, guint range_count)
{
  for(size_t i = 0; i < size; i++)
    if(before[i] != after[i] && !byte_offset_is_native(ranges, range_count, i))
      return FALSE;
  return TRUE;
}

gboolean dt_remote_quantity_apply_entries(const struct dt_iop_module_t *module,
                                          const void *old_params,
                                          void *new_params,
                                          GPtrArray *entries,
                                          const dt_remote_patch_t *patch,
                                          dt_remote_error_t **error)
{
  // `old_params` is otherwise unused: kept for signature symmetry with the
  // curve/vector/band twins; this engine has no native leaf of its own to
  // read pre-transaction state from (see this function's own doc comment
  // in remote_quantity.h).
  if(!module || !module->so || !old_params || !new_params || !entries)
  {
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: null argument to quantity mutation engine")), error);
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection
    ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: module '%s' has no quantity introspection"), module->op),
                           error);
    return FALSE;
  }

  const dt_remote_quantity_module_adapter_t *adapter =
    dt_remote_quantity_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    if(entries->len == 0) return TRUE;
    const dt_remote_semantic_patch_t *first = g_ptr_array_index(entries, 0);
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_UNKNOWN_FIELD,
                             _("unknown semantic quantity '%s'"),
                             first->value.quantity.name ? first->value.quantity.name : ""),
                           error);
    return FALSE;
  }

  if(!dt_remote_quantity_registry_validate(adapter, module->so, error)) return FALSE;

  // Resolve every entry's ID against the adapter, and reject a repeated
  // name, before any value validation or write below -- same two-pass
  // shape as remote_band.c's dt_remote_band_apply_entries().
  for(guint i = 0; i < entries->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(entries, i);
    g_assert(semantic && semantic->class_id == DT_REMOTE_PARAMETER_QUANTITY);

    const char *name = semantic->value.quantity.name;
    if(!find_quantity_descriptor(adapter, name))
    {
      deliver_quantity_error(dt_remote_quantity_error_new(
                               DT_REMOTE_ERR_UNKNOWN_FIELD,
                               _("unknown semantic quantity '%s'"), name ? name : ""), error);
      return FALSE;
    }
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_semantic_patch_t *prior = g_ptr_array_index(entries, j);
      if(!g_strcmp0(prior->value.quantity.name, name))
      {
        deliver_quantity_error(quantity_parameter_error_new(
                                 DT_REMOTE_ERR_INVALID_VALUE, name, "duplicate_parameter",
                                 _("duplicate semantic quantity '%s'"), name), error);
        return FALSE;
      }
    }
  }

  // Validate every entry's component values in isolation BEFORE any of
  // them writes anything: a later entry's rejection must never leave an
  // earlier entry's write in `new_params`.
  for(guint i = 0; i < entries->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(entries, i);
    const dt_remote_quantity_descriptor_t *desc =
      find_quantity_descriptor(adapter, semantic->value.quantity.name);
    if(!dt_remote_quantity_validate(desc, semantic->value.quantity.values, error)) return FALSE;
  }

  // Native-conflict rule (the "coefficient coexistence" exception's
  // enforcement point, see remote_quantity.h's own top comment): at least
  // one quantity entry targets this adapter (true whenever `entries` is
  // nonempty, which it is past this point -- see the zero-entries
  // short-circuit below) and `patch`'s scalar_values names one of the
  // adapter's native fields.
  if(entries->len > 0 && patch && patch->scalar_values)
  {
    for(guint i = 0; i < patch->scalar_values->len; i++)
    {
      const dt_remote_patch_entry_t *scalar_entry = g_ptr_array_index(patch->scalar_values, i);
      if(!scalar_entry) continue;
      for(guint k = 0; k < adapter->native_field_count; k++)
      {
        if(!g_strcmp0(scalar_entry->name, adapter->native_fields[k]))
        {
          deliver_quantity_error(quantity_parameter_error_new(
                                   DT_REMOTE_ERR_INVALID_VALUE, scalar_entry->name, "native_conflict",
                                   _("scalar field '%s' cannot be written together with a quantity "
                                     "entry that represents it"),
                                   scalar_entry->name),
                                 error);
          return FALSE;
        }
      }
    }
  }

  if(entries->len == 0) return TRUE;

  // Resolve the native-field byte ranges once, before any write -- reused
  // by the byte-check after every descriptor's write below.
  native_field_range_t *ranges = g_new(native_field_range_t, adapter->native_field_count);
  if(!resolve_native_field_ranges(adapter, intro->field, new_params, ranges, error))
  {
    g_free(ranges);
    return FALSE;
  }

  // Apply in descriptor/registry order, never request/array order.
  for(guint descriptor_index = 0; descriptor_index < adapter->quantity_count; descriptor_index++)
  {
    const dt_remote_quantity_descriptor_t *desc = &adapter->quantities[descriptor_index];
    const dt_remote_quantity_patch_t *entry = find_quantity_entry(entries, desc->name);
    if(!entry) continue;

    gboolean active = TRUE;
    gboolean writable = TRUE;
    if(!quantity_predicate_holds(desc->active_when, intro->field, new_params, &active, error)
       || !quantity_predicate_holds(desc->writable_when, intro->field, new_params, &writable, error))
    {
      g_free(ranges);
      return FALSE;
    }
    if(!active || !writable)
    {
      deliver_quantity_error(quantity_parameter_error_new(
                               DT_REMOTE_ERR_UNSUPPORTED_FIELD, desc->name, "condition_not_satisfied",
                               _("semantic quantity '%s' is inactive or not writable in projected state"),
                               desc->name),
                             error);
      g_free(ranges);
      return FALSE;
    }

    // Component values in descriptor order -- dt_remote_quantity_validate()
    // above already proved `entry->values` names exactly `desc`'s component
    // set, so every lookup below is guaranteed to find its component.
    double *values = g_new(double, desc->component_count);
    for(guint c = 0; c < desc->component_count; c++)
    {
      const char *component_name = desc->components[c].name;
      double value = 0.0;
      for(guint i = 0; i < entry->values->len; i++)
      {
        const dt_remote_quantity_component_value_t *item = g_ptr_array_index(entry->values, i);
        if(!g_strcmp0(item->name, component_name)) { value = item->value; break; }
      }
      values[c] = value;
    }

    guint8 *before = g_malloc(module->params_size);
    memcpy(before, new_params, module->params_size);

    dt_remote_quantity_write_hook_t override_hook = dt_remote_quantity_registry_current_write_hook();
    struct dt_iop_module_t *mutable_module = (struct dt_iop_module_t *)module;
    const gboolean ok = override_hook
      ? override_hook(mutable_module, values, desc->component_count, new_params)
      : (module->so->remote_quantity_write
           ? module->so->remote_quantity_write(mutable_module, values, desc->component_count, new_params)
           : FALSE);
    g_free(values);

    if(!ok)
    {
      memcpy(new_params, before, module->params_size);
      g_free(before);
      g_free(ranges);
      deliver_quantity_error(dt_remote_quantity_error_new(
                               DT_REMOTE_ERR_INTERNAL,
                               _("internal error: quantity '%s' conversion write hook failed"), desc->name),
                             error);
      return FALSE;
    }

    if(!only_native_bytes_changed(before, (const guint8 *)new_params, module->params_size,
                                  ranges, adapter->native_field_count))
    {
      memcpy(new_params, before, module->params_size);
      g_free(before);
      g_free(ranges);
      deliver_quantity_error(dt_remote_quantity_error_new(
                               DT_REMOTE_ERR_INTERNAL,
                               _("internal error: quantity '%s' conversion write hook modified "
                                 "unexpected bytes"),
                               desc->name),
                             error);
      return FALSE;
    }

    g_free(before);
  }

  g_free(ranges);
  return TRUE;
}

gboolean dt_remote_quantity_apply_patch(const struct dt_iop_module_t *module,
                                        const void *old_params,
                                        void *new_params,
                                        const dt_remote_patch_t *patch,
                                        dt_remote_error_t **error)
{
  if(!module || !module->so || !old_params || !new_params || !patch)
  {
    deliver_quantity_error(dt_remote_quantity_error_new(
                             DT_REMOTE_ERR_INTERNAL,
                             _("internal error: null argument to quantity mutation engine")), error);
    return FALSE;
  }

  // Partition this class's entries out of the patch, in request order, and
  // hand the slice to the self-contained engine above -- entries are
  // borrowed from `patch`, never owned or freed by this array. Unlike the
  // band/vector twins there is no prepare_fields gate to evaluate first (a
  // quantity adapter declares none): a patch with zero quantity entries
  // returns TRUE immediately, without ever looking up or validating the
  // registry.
  GPtrArray *entries = g_ptr_array_new();
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(!semantic)
    {
      deliver_quantity_error(dt_remote_quantity_error_new(
                               DT_REMOTE_ERR_INTERNAL,
                               _("internal error: null semantic patch entry")), error);
      g_ptr_array_unref(entries);
      return FALSE;
    }
    if(semantic->class_id == DT_REMOTE_PARAMETER_QUANTITY)
      g_ptr_array_add(entries, semantic);
  }

  if(entries->len == 0)
  {
    g_ptr_array_unref(entries);
    return TRUE;
  }

  const gboolean ok =
    dt_remote_quantity_apply_entries(module, old_params, new_params, entries, patch, error);
  g_ptr_array_unref(entries);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
