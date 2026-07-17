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

#include "control/remote_band.h"

#include "common/darktable.h" // _()
#include "develop/imageop.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_vector.c's own file-local idiom: see that  */
/* file's comment)                                                         */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_band_error_new(dt_remote_error_code_t code,
                                                    const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_band_error_new(dt_remote_error_code_t code,
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
// "parameter" (always), "array" (always, "y" or "x"), "constraint"
// (always, one of the short stable strings documented on
// dt_remote_band_validate()), and "index" (only when index >= 0) -- same
// convention as dt_remote_vector_validate_error_new() in remote_vector.c,
// extended with an "array" member since a band-set has two arrays a
// violation can be attributed to.
static dt_remote_error_t *dt_remote_band_validate_error_new(const char *parameter, const char *array,
                                                             int index, const char *constraint,
                                                             const char *format, ...)
  G_GNUC_PRINTF(5, 6);

static dt_remote_error_t *dt_remote_band_validate_error_new(const char *parameter, const char *array,
                                                             int index, const char *constraint,
                                                             const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = DT_REMOTE_ERR_INVALID_VALUE;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  if(index >= 0)
    error->details_json = g_strdup_printf(
      "{\"parameter\":\"%s\",\"array\":\"%s\",\"index\":%d,\"constraint\":\"%s\"}",
      parameter ? parameter : "", array, index, constraint);
  else
    error->details_json = g_strdup_printf(
      "{\"parameter\":\"%s\",\"array\":\"%s\",\"constraint\":\"%s\"}",
      parameter ? parameter : "", array, constraint);

  return error;
}

static void deliver_band_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

static dt_remote_error_t *band_parameter_error_new(dt_remote_error_code_t code,
                                                    const char *parameter,
                                                    const char *constraint,
                                                    const char *format, ...)
  G_GNUC_PRINTF(4, 5);

static dt_remote_error_t *band_parameter_error_new(dt_remote_error_code_t code,
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
/* common band validator                                                   */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_band_validate(const dt_remote_band_descriptor_t *desc,
                                 const GArray *y,
                                 const GArray *x,
                                 const float *stored_x,
                                 dt_remote_error_t **error)
{
  if(!desc)
  {
    if(error)
      *error = dt_remote_band_error_new(DT_REMOTE_ERR_INTERNAL,
                                        _("internal error: null band descriptor"));
    return FALSE;
  }

  const char *parameter = desc->name ? desc->name : "";
  const guint yn = y ? y->len : 0;

  if(yn != desc->count)
  {
    if(error)
      *error = dt_remote_band_validate_error_new(
        parameter, "y", -1, "count_mismatch",
        _("band '%s' has %u y sample(s); %u required"), parameter, yn, desc->count);
    return FALSE;
  }

  for(guint i = 0; i < yn; i++)
  {
    const double value = g_array_index(y, double, i);
    if(!isfinite(value))
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "y", (int)i, "non_finite",
          _("band '%s' y sample %u has a non-finite value"), parameter, i);
      return FALSE;
    }
    if(value < desc->y_minimum || value > desc->y_maximum)
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "y", (int)i, "domain",
          _("band '%s' y sample %u has value=%.17g outside domain [%.17g, %.17g]"),
          parameter, i, value, desc->y_minimum, desc->y_maximum);
      return FALSE;
    }
    // Defensive preflight, independent of `desc`'s own bounds -- see
    // dt_remote_vector_validate()'s matching check for the rationale
    // (this function is documented as pure and directly callable, so a
    // caller-supplied descriptor is not guaranteed to have gone through
    // dt_remote_band_registry_validate()'s own bound cap).
    if(value < -(double)FLT_MAX || value > (double)FLT_MAX)
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "y", (int)i, "native_range",
          _("band '%s' y sample %u has value=%.17g outside the representable native float range "
            "[%.17g, %.17g]"),
          parameter, i, value, -(double)FLT_MAX, (double)FLT_MAX);
      return FALSE;
    }
  }

  if(x && desc->x_policy == DT_REMOTE_BAND_X_INTERIOR)
  {
    const guint xn = x->len;
    if(xn != desc->count)
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "x", -1, "count_mismatch",
          _("band '%s' has %u x sample(s); %u required"), parameter, xn, desc->count);
      return FALSE;
    }

    const float first_narrowed = (float)g_array_index(x, double, 0);
    const float last_narrowed = (float)g_array_index(x, double, xn - 1);
    // `stored_x` is always supplied by dt_remote_band_apply_entries() when
    // `x` is non-NULL under INTERIOR; a direct pure-function caller that
    // omits it cannot have its endpoints checked meaningfully, so treat
    // that as a (defensive) endpoint violation at index 0.
    if(!stored_x)
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "x", 0, "endpoint",
          _("band '%s' x endpoints cannot be checked without stored positions"), parameter);
      return FALSE;
    }
    if(first_narrowed != stored_x[0])
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "x", 0, "endpoint",
          _("band '%s' x endpoint 0 does not match the currently stored pinned position"), parameter);
      return FALSE;
    }
    if(last_narrowed != stored_x[xn - 1])
    {
      if(error)
        *error = dt_remote_band_validate_error_new(
          parameter, "x", (int)(xn - 1), "endpoint",
          _("band '%s' x endpoint %u does not match the currently stored pinned position"),
          parameter, xn - 1);
      return FALSE;
    }

    for(guint i = 1; i < xn; i++)
    {
      const double previous = g_array_index(x, double, i - 1);
      const double current = g_array_index(x, double, i);
      const double delta = current - previous;

      if(delta <= 0.0)
      {
        if(error)
          *error = dt_remote_band_validate_error_new(
            parameter, "x", (int)i, "unordered",
            _("band '%s' x samples %u and %u are not strictly increasing"), parameter, i - 1, i);
        return FALSE;
      }
      if(delta < desc->minimum_gap)
      {
        if(error)
          *error = dt_remote_band_validate_error_new(
            parameter, "x", (int)i, "gap",
            _("band '%s' x samples %u and %u are %.17g apart; minimum is %.17g"),
            parameter, i - 1, i, delta, desc->minimum_gap);
        return FALSE;
      }
    }
  }

  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* semantic band mutation engine                                           */
/* ---------------------------------------------------------------------- */

static gboolean band_predicate_holds(const dt_remote_parameter_predicate_t *predicate,
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
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: band predicate '%s' drifted from introspection"),
                         predicate->field ? predicate->field : ""),
                       error);
    return FALSE;
  }

  const char *live_name = dt_introspection_get_enum_name((dt_introspection_field_t *)field,
                                                          *(const int *)ptr);
  if(!live_name)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: band predicate '%s' has an unknown enum value"),
                         predicate->field),
                       error);
    return FALSE;
  }
  const gboolean equal = !g_strcmp0(live_name, predicate->enum_name);
  *out = predicate->op == DT_REMOTE_PREDICATE_EQ ? equal : !equal;
  return TRUE;
}

static const dt_remote_band_descriptor_t *find_band_descriptor(
  const dt_remote_band_module_adapter_t *adapter,
  const char *name)
{
  if(!adapter || !name) return NULL;
  for(guint i = 0; i < adapter->band_count; i++)
    if(!g_strcmp0(adapter->bands[i].name, name)) return &adapter->bands[i];
  return NULL;
}

static const dt_remote_band_patch_t *find_band_entry(GPtrArray *entries, const char *name)
{
  if(!entries || !name) return NULL;
  for(guint i = 0; i < entries->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(entries, i);
    if(semantic && !g_strcmp0(semantic->value.bands.name, name))
      return &semantic->value.bands;
  }
  return NULL;
}

// `adapter->prepare_field_count > 0` with `adapter->prepare_fields == NULL`
// is a malformed adapter envelope -- the same shape rejected by registry
// validation, but this helper runs before that registry validation to
// compute the prepare_needed fast-path gate. Detect it here and report
// through `*out_malformed` before ever indexing `adapter->prepare_fields`,
// mirroring patch_mentions_prepare_field() in remote_vector.c exactly.
static gboolean patch_mentions_prepare_field(const dt_remote_band_module_adapter_t *adapter,
                                             const dt_remote_patch_t *patch,
                                             gboolean *out_malformed)
{
  if(out_malformed) *out_malformed = FALSE;
  if(adapter->prepare_field_count > 0 && !adapter->prepare_fields)
  {
    if(out_malformed) *out_malformed = TRUE;
    return FALSE;
  }

  for(guint i = 0; patch->scalar_values && i < patch->scalar_values->len; i++)
  {
    const dt_remote_patch_entry_t *entry = g_ptr_array_index(patch->scalar_values, i);
    for(guint j = 0; entry && j < adapter->prepare_field_count; j++)
      if(!g_strcmp0(entry->name, adapter->prepare_fields[j])) return TRUE;
  }
  return FALSE;
}

// Resolves `desc`'s native_x leaf and widens its current `desc->count`
// floats into `out_stored_x` (caller-owned, sized desc->count). Only
// meaningful under INTERIOR (FIXED descriptors are never endpoint-checked),
// but resolved unconditionally when requested since native_x always exists
// on every descriptor regardless of policy.
static gboolean read_stored_x(const dt_remote_band_descriptor_t *desc,
                              const dt_introspection_field_t *root,
                              void *params,
                              float *out_stored_x,
                              dt_remote_error_t **error)
{
  const dt_introspection_field_t *array_field = NULL;
  void *array_ptr = NULL;
  if(!dt_remote_path_resolve(&desc->native_x, root, params, &array_field, &array_ptr, error))
    return FALSE;
  if(!array_field || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: band '%s' native_x layout drifted"), desc->name),
                       error);
    return FALSE;
  }

  for(guint i = 0; i < desc->count; i++)
  {
    dt_introspection_field_t *element_field = NULL;
    void *element_ptr = dt_introspection_access_array((dt_introspection_field_t *)array_field,
                                                       array_ptr, i, &element_field);
    if(!element_ptr || !element_field || element_field->header.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      deliver_band_error(dt_remote_band_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: band '%s' native_x component %u drifted"), desc->name, i),
                         error);
      return FALSE;
    }
    out_stored_x[i] = *(const float *)element_ptr;
  }
  return TRUE;
}

// Resolves `path` (native_x or native_y) to its float array leaf and
// element pointers, filling `out_element_ptrs[0..desc->count)`.
static gboolean resolve_band_array(const dt_remote_band_descriptor_t *desc,
                                   const dt_remote_introspection_path_t *path,
                                   const char *which,
                                   const dt_introspection_field_t *root,
                                   void *params,
                                   void **out_element_ptrs,
                                   dt_remote_error_t **error)
{
  const dt_introspection_field_t *array_field = NULL;
  void *array_ptr = NULL;
  if(!dt_remote_path_resolve(path, root, params, &array_field, &array_ptr, error)) return FALSE;
  if(!array_field || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: band '%s' native_%s layout drifted"), desc->name, which),
                       error);
    return FALSE;
  }

  for(guint i = 0; i < desc->count; i++)
  {
    dt_introspection_field_t *element_field = NULL;
    void *element_ptr = dt_introspection_access_array((dt_introspection_field_t *)array_field,
                                                       array_ptr, i, &element_field);
    if(!element_ptr || !element_field || element_field->header.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      deliver_band_error(dt_remote_band_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: band '%s' native_%s component %u drifted"),
                           desc->name, which, i),
                         error);
      return FALSE;
    }
    out_element_ptrs[i] = element_ptr;
  }
  return TRUE;
}

// Validates one band entry's VALUE-level correctness only: x-policy gate,
// dt_remote_band_validate() in the double domain, and twin-conflict
// detection. Deliberately touches neither `new_params` (no writes) nor any
// predicate (active_when/writable_when depend on the *projected* state,
// which is this call's own responsibility as earlier entries are written --
// see write_band_patch() below), so this can run for every entry in
// `entries` before any of them writes anything -- see
// dt_remote_band_apply_entries()'s own doc comment for why that ordering
// matters.
//
// Endpoint pinning is checked against `old_params` (the genuinely
// pre-transaction block), never `new_params`: this function runs for every
// entry in `entries` before ANY of them has written anything (called from
// a dedicated pre-pass in dt_remote_band_apply_entries(), never
// interleaved with writes), so `old_params` and `new_params` are
// equivalent for band-native purposes at the point this runs -- but
// `old_params` is used explicitly (rather than relying on that
// invariant) because it is what the type signature documents as
// immutable, and because a future refactor that interleaves this pre-pass
// with writes must not silently reintroduce the vacuous-check bug this
// fixes: reading a "currently stored" endpoint from a block a twin may
// already have mirrored into would make the check compare the submitted x
// against a value a twin just wrote, not this descriptor's true
// pre-transaction endpoint.
static gboolean validate_band_entry(const dt_remote_band_module_adapter_t *adapter,
                                    const dt_remote_band_descriptor_t *desc,
                                    const dt_introspection_field_t *root,
                                    const void *old_params,
                                    GPtrArray *entries,
                                    const dt_remote_band_patch_t *band_patch,
                                    dt_remote_error_t **error)
{
  if(band_patch->x && desc->x_policy == DT_REMOTE_BAND_X_FIXED)
  {
    deliver_band_error(band_parameter_error_new(
                         DT_REMOTE_ERR_UNSUPPORTED_FIELD, desc->name, "x_not_supported",
                         _("semantic band '%s' does not accept x under its FIXED x policy"),
                         desc->name),
                       error);
    return FALSE;
  }

  float *stored_x = NULL;
  if(band_patch->x && desc->x_policy == DT_REMOTE_BAND_X_INTERIOR)
  {
    stored_x = g_new(float, desc->count);
    if(!read_stored_x(desc, root, (void *)old_params, stored_x, error))
    {
      g_free(stored_x);
      return FALSE;
    }
  }

  if(!dt_remote_band_validate(desc, band_patch->y, band_patch->x, stored_x, error))
  {
    g_free(stored_x);
    return FALSE;
  }
  g_free(stored_x);

  const dt_remote_band_descriptor_t *twin =
    desc->x_shared_with ? find_band_descriptor(adapter, desc->x_shared_with) : NULL;

  if(band_patch->x && twin)
  {
    const dt_remote_band_patch_t *twin_entry = find_band_entry(entries, twin->name);
    if(twin_entry && twin_entry->x)
    {
      gboolean mismatch = twin_entry->x->len != band_patch->x->len;
      for(guint i = 0; !mismatch && i < band_patch->x->len; i++)
        if(g_array_index(band_patch->x, double, i) != g_array_index(twin_entry->x, double, i))
          mismatch = TRUE;
      if(mismatch)
      {
        deliver_band_error(band_parameter_error_new(
                             DT_REMOTE_ERR_INVALID_VALUE, desc->name, "twin_conflict",
                             _("semantic bands '%s' and '%s' share x but this request supplies "
                               "differing x values for it"),
                             desc->name, twin->name),
                           error);
        return FALSE;
      }
    }
  }

  return TRUE;
}

// Applies one already-validated band entry: predicate gating (against the
// *projected* `new_params` -- may already reflect earlier entries' writes
// in this same call, "vector engine ordering"), then narrowing writes
// (native_y always; native_x, and the twin's native_x, when `x` is given).
// Every value-level check (x-policy gate, dt_remote_band_validate(),
// twin-conflict) has already passed via validate_band_entry() above, run
// for every entry in `entries` before this function is ever called for
// any of them -- see dt_remote_band_apply_entries()'s own doc comment for
// the full behavioral contract this implements.
static gboolean write_band_patch(const dt_remote_band_module_adapter_t *adapter,
                                 const dt_remote_band_descriptor_t *desc,
                                 const dt_introspection_field_t *root,
                                 void *new_params,
                                 const dt_remote_band_patch_t *band_patch,
                                 dt_remote_error_t **error)
{
  gboolean active = TRUE;
  gboolean writable = TRUE;
  if(!band_predicate_holds(desc->active_when, root, new_params, &active, error)
     || !band_predicate_holds(desc->writable_when, root, new_params, &writable, error))
    return FALSE;
  if(!active || !writable)
  {
    deliver_band_error(band_parameter_error_new(
                         DT_REMOTE_ERR_UNSUPPORTED_FIELD, desc->name, "condition_not_satisfied",
                         _("semantic band '%s' is inactive or not writable in projected state"),
                         desc->name),
                       error);
    return FALSE;
  }

  const dt_remote_band_descriptor_t *twin =
    desc->x_shared_with ? find_band_descriptor(adapter, desc->x_shared_with) : NULL;

  // Preflight: resolve every destination element before writing any of
  // them, so a mid-write introspection drift never leaves a partial write
  // -- same discipline as write_vector_patch() in remote_vector.c.
  void **y_ptrs = g_new(void *, desc->count);
  if(!resolve_band_array(desc, &desc->native_y, "y", root, new_params, y_ptrs, error))
  {
    g_free(y_ptrs);
    return FALSE;
  }

  void **x_ptrs = NULL;
  void **twin_x_ptrs = NULL;
  if(band_patch->x)
  {
    x_ptrs = g_new(void *, desc->count);
    if(!resolve_band_array(desc, &desc->native_x, "x", root, new_params, x_ptrs, error))
    {
      g_free(y_ptrs);
      g_free(x_ptrs);
      return FALSE;
    }
    if(twin)
    {
      twin_x_ptrs = g_new(void *, twin->count);
      if(!resolve_band_array(twin, &twin->native_x, "x", root, new_params, twin_x_ptrs, error))
      {
        g_free(y_ptrs);
        g_free(x_ptrs);
        g_free(twin_x_ptrs);
        return FALSE;
      }
    }
  }

  for(guint i = 0; i < desc->count; i++)
    *(float *)y_ptrs[i] = (float)g_array_index(band_patch->y, double, i);
  if(x_ptrs)
  {
    for(guint i = 0; i < desc->count; i++)
    {
      const float narrowed = (float)g_array_index(band_patch->x, double, i);
      *(float *)x_ptrs[i] = narrowed;
      if(twin_x_ptrs) *(float *)twin_x_ptrs[i] = narrowed;
    }
  }

  g_free(y_ptrs);
  g_free(x_ptrs);
  g_free(twin_x_ptrs);
  return TRUE;
}

gboolean dt_remote_band_apply_entries(const struct dt_iop_module_t *module,
                                      const void *old_params,
                                      void *new_params,
                                      GPtrArray *entries,
                                      dt_remote_error_t **error)
{
  if(!module || !module->so || !old_params || !new_params || !entries)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: null argument to band mutation engine")), error);
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection
    ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: module '%s' has no band introspection"), module->op),
                       error);
    return FALSE;
  }

  const dt_remote_band_module_adapter_t *adapter =
    dt_remote_band_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    if(entries->len == 0) return TRUE;
    const dt_remote_semantic_patch_t *first = g_ptr_array_index(entries, 0);
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_UNKNOWN_FIELD,
                         _("unknown semantic band '%s'"),
                         first->value.bands.name ? first->value.bands.name : ""),
                       error);
    return FALSE;
  }

  if(!dt_remote_band_registry_validate(adapter, module->so, error)) return FALSE;

  const dt_remote_band_context_t context = {
    .module = module, .introspection = intro, .adapter = adapter,
  };

  // Resolve every entry's ID against the adapter, and reject a repeated
  // name, before the registry-ordered write loop below -- same two-pass
  // shape as remote_vector.c's dt_remote_vector_apply_entries().
  for(guint i = 0; i < entries->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(entries, i);
    g_assert(semantic && semantic->class_id == DT_REMOTE_PARAMETER_BANDS);

    const char *name = semantic->value.bands.name;
    if(!find_band_descriptor(adapter, name))
    {
      deliver_band_error(dt_remote_band_error_new(
                           DT_REMOTE_ERR_UNKNOWN_FIELD,
                           _("unknown semantic band '%s'"), name ? name : ""), error);
      return FALSE;
    }
    for(guint j = 0; j < i; j++)
    {
      const dt_remote_semantic_patch_t *prior = g_ptr_array_index(entries, j);
      if(!g_strcmp0(prior->value.bands.name, name))
      {
        deliver_band_error(band_parameter_error_new(
                             DT_REMOTE_ERR_INVALID_VALUE, name, "duplicate_parameter",
                             _("duplicate semantic band '%s'"), name), error);
        return FALSE;
      }
    }
  }

  // Validate every entry's VALUE-level correctness (x-policy gate,
  // dt_remote_band_validate() against `old_params`-stored endpoints,
  // twin-conflict) in registry order BEFORE any of them writes anything --
  // see validate_band_entry()'s own doc comment for why this must be a
  // separate pass, not interleaved with the write loop below: without it,
  // an earlier twin's mirror write could reach a later twin's endpoint
  // check before that check runs, and this pass is also what makes a
  // multi-entry rejection here leave `new_params` completely untouched
  // (the only remaining source of a partial write on rejection is the
  // predicate gate in the write loop below, whose "vector engine ordering"
  // dependency on the projected block cannot be resolved without writing
  // -- same caveat dt_remote_vector_apply_entries() documents).
  for(guint descriptor_index = 0; descriptor_index < adapter->band_count; descriptor_index++)
  {
    const dt_remote_band_descriptor_t *desc = &adapter->bands[descriptor_index];
    const dt_remote_band_patch_t *band_patch = find_band_entry(entries, desc->name);
    if(!band_patch) continue;

    if(!validate_band_entry(adapter, desc, intro->field, old_params, entries, band_patch, error))
      return FALSE;
  }

  // Apply in descriptor/registry order, never request hash/array order.
  for(guint descriptor_index = 0; descriptor_index < adapter->band_count; descriptor_index++)
  {
    const dt_remote_band_descriptor_t *desc = &adapter->bands[descriptor_index];
    const dt_remote_band_patch_t *band_patch = find_band_entry(entries, desc->name);
    if(!band_patch) continue;

    if(!write_band_patch(adapter, desc, intro->field, new_params, band_patch, error))
      return FALSE;
  }

  if(adapter->validate_completed
     && !adapter->validate_completed(&context, new_params, error))
    return FALSE;
  return TRUE;
}

gboolean dt_remote_band_apply_patch(const struct dt_iop_module_t *module,
                                    const void *old_params,
                                    void *new_params,
                                    const dt_remote_patch_t *patch,
                                    dt_remote_error_t **error)
{
  if(!module || !module->so || !old_params || !new_params || !patch)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: null argument to band mutation engine")), error);
    return FALSE;
  }

  gboolean has_band_semantics = FALSE;
  const char *first_band_name = NULL;
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    const dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(semantic && semantic->class_id == DT_REMOTE_PARAMETER_BANDS)
    {
      has_band_semantics = TRUE;
      first_band_name = semantic->value.bands.name;
      break;
    }
  }

  dt_introspection_t *intro = module->so->get_introspection
    ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: module '%s' has no band introspection"), module->op),
                       error);
    return FALSE;
  }

  const dt_remote_band_module_adapter_t *adapter =
    dt_remote_band_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    if(!has_band_semantics) return TRUE;
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_UNKNOWN_FIELD,
                         _("unknown semantic band '%s'"),
                         first_band_name ? first_band_name : ""),
                       error);
    return FALSE;
  }

  // Preserved short-circuit: when `has_band_semantics` is already TRUE,
  // full registry validation runs unconditionally below (inside
  // dt_remote_band_apply_entries()) and will itself reject a malformed
  // prepare-fields envelope, so the traversal (and its malformed-envelope
  // probe) is skipped entirely.
  gboolean prepare_fields_malformed = FALSE;
  const gboolean mentions_prepare_field = has_band_semantics
    ? FALSE
    : patch_mentions_prepare_field(adapter, patch, &prepare_fields_malformed);
  if(prepare_fields_malformed)
  {
    deliver_band_error(dt_remote_band_error_new(
                         DT_REMOTE_ERR_INTERNAL,
                         _("internal error: band adapter '%s' has a malformed prepare-fields envelope"),
                         adapter->operation),
                       error);
    return FALSE;
  }
  const gboolean prepare_needed = has_band_semantics || mentions_prepare_field;
  if(!prepare_needed) return TRUE;

  // Partition this class's entries out of the patch, in request order, and
  // hand the slice to the self-contained engine above -- entries are
  // borrowed from `patch`, never owned or freed by this array.
  GPtrArray *entries = g_ptr_array_new();
  for(guint i = 0; patch->semantic_values && i < patch->semantic_values->len; i++)
  {
    dt_remote_semantic_patch_t *semantic = g_ptr_array_index(patch->semantic_values, i);
    if(!semantic)
    {
      deliver_band_error(dt_remote_band_error_new(
                           DT_REMOTE_ERR_INTERNAL,
                           _("internal error: null semantic patch entry")), error);
      g_ptr_array_unref(entries);
      return FALSE;
    }
    if(semantic->class_id == DT_REMOTE_PARAMETER_BANDS)
      g_ptr_array_add(entries, semantic);
  }

  const gboolean ok = dt_remote_band_apply_entries(module, old_params, new_params, entries, error);
  g_ptr_array_unref(entries);
  return ok;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
