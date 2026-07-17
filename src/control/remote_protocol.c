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

#include "control/remote_protocol.h"

#include "common/darktable.h"  // for the _() gettext macro
#include "control/jobs.h"           // DT_JOB_QUEUE_SYSTEM_BG render job
#include "control/remote_frame.h"   // DT_REMOTE_MAX_FRAME (proactive size check)
#include "control/remote_parameters.h" // semantic curve schema/value types (milestone 2)
#include "control/remote_server.h"  // dt_remote_async_* (production async table)

#include <math.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* tunables                                                                */
/* ---------------------------------------------------------------------- */

// Generic sanity cap on any single string field (module/op names, hello's
// token/client, ...), independent of the 16 MiB whole-frame cap the framing
// layer enforces later -- defense in depth against one absurd field.
#define DT_REMOTE_PROTOCOL_MAX_STRING_LEN 4096

/* ---------------------------------------------------------------------- */
/* remote-edit call table (test seam)                                      */
/* ---------------------------------------------------------------------- */

static const dt_remote_protocol_calls_t DEFAULT_CALLS = {
  .get_state = dt_remote_get_state,
  .list_modules = dt_remote_list_modules,
  .get_module_schema = dt_remote_get_module_schema,
  .get_module_primitive_schema = dt_remote_get_module_primitive_schema,
  .get_module_params = dt_remote_get_module_params,
  .set_module_params = dt_remote_set_module_params,
  .set_module_enabled = dt_remote_set_module_enabled,
  .reset_module = dt_remote_reset_module,
  .create_module_instance = dt_remote_create_module_instance,
  .get_history = dt_remote_get_history,
  .undo = dt_remote_undo,
  .render_preview_prepare = dt_remote_render_preview_prepare,
  .current_revision = dt_remote_current_revision,
  .scopes_prepare = dt_remote_scopes_prepare,
};

static dt_remote_protocol_calls_t s_calls = {
  .get_state = dt_remote_get_state,
  .list_modules = dt_remote_list_modules,
  .get_module_schema = dt_remote_get_module_schema,
  .get_module_primitive_schema = dt_remote_get_module_primitive_schema,
  .get_module_params = dt_remote_get_module_params,
  .set_module_params = dt_remote_set_module_params,
  .set_module_enabled = dt_remote_set_module_enabled,
  .reset_module = dt_remote_reset_module,
  .create_module_instance = dt_remote_create_module_instance,
  .get_history = dt_remote_get_history,
  .undo = dt_remote_undo,
  .render_preview_prepare = dt_remote_render_preview_prepare,
  .current_revision = dt_remote_current_revision,
  .scopes_prepare = dt_remote_scopes_prepare,
};

void dt_remote_protocol_set_calls(const dt_remote_protocol_calls_t *calls)
{
  s_calls = calls ? *calls : DEFAULT_CALLS;
}

/* ---------------------------------------------------------------------- */
/* async transport table (test seam)                                       */
/* ---------------------------------------------------------------------- */

// production queue_preview: a DT_JOB_QUEUE_SYSTEM_BG job defined with the
// rest of the render plumbing further down
static gboolean _queue_preview_job(dt_remote_pending_t *pending,
                                   const dt_remote_preview_request_t *req,
                                   int max_px, int quality);
static gboolean _queue_scopes_job(dt_remote_pending_t *pending,
                                  const dt_remote_scopes_request_t *req);

static const dt_remote_protocol_async_t DEFAULT_ASYNC = {
  .begin = dt_remote_async_begin,
  .abort = dt_remote_async_abort,
  .complete = dt_remote_async_complete,
  .queue_preview = _queue_preview_job,
  .queue_scopes = _queue_scopes_job,
};

static dt_remote_protocol_async_t s_async = {
  .begin = dt_remote_async_begin,
  .abort = dt_remote_async_abort,
  .complete = dt_remote_async_complete,
  .queue_preview = _queue_preview_job,
  .queue_scopes = _queue_scopes_job,
};

void dt_remote_protocol_set_async(const dt_remote_protocol_async_t *ops)
{
  s_async = ops ? *ops : DEFAULT_ASYNC;
}

/* ---------------------------------------------------------------------- */
/* handler failure signaling                                                */
/* ---------------------------------------------------------------------- */

// A handler returns NULL to signal failure and stashes the error here
// immediately beforehand; dispatch() reads and clears it right after the
// handler call returns. Safe because handlers run inline, synchronously,
// on a single thread (internals §1: the dispatcher never re-enters itself
// and nothing else touches this global) -- there is deliberately no
// out-parameter on the handler signature (it is normative, see the
// header), so this is the simplest way to carry a dt_remote_error_t out
// of a `JsonNode *(*)(...)` function.
static dt_remote_error_t *s_handler_error = NULL;

static JsonNode *_handler_fail(dt_remote_error_t *err)
{
  g_clear_pointer(&s_handler_error, dt_remote_error_free);
  s_handler_error = err;
  return NULL;
}

static dt_remote_error_t *_error_new(dt_remote_error_code_t code, const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *_error_new(dt_remote_error_code_t code, const char *format, ...)
{
  dt_remote_error_t *err = g_malloc0(sizeof(dt_remote_error_t));
  err->code = code;

  va_list args;
  va_start(args, format);
  err->message = g_strdup_vprintf(format, args);
  va_end(args);

  return err;
}

/* ---------------------------------------------------------------------- */
/* parameter validation helpers                                            */
/* ---------------------------------------------------------------------- */

// Accepts a JSON integer value, or a JSON floating-point value that is
// finite and has no fractional part (so `"id": 1.0` is as valid as
// `"id": 1`, per the protocol reference's "integer fields reject
// fractional values" rule -- rejecting *fractional* values, not merely
// the JSON literal spelling). Rejects non-finite doubles (e.g. a JSON
// literal like `1e400` that overflows to +Inf on parse), out-of-range
// values, and any non-numeric node.
static gboolean _json_node_get_integer(JsonNode *node, gint64 *out)
{
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;

  GType type = json_node_get_value_type(node);
  if(type == G_TYPE_INT64)
  {
    *out = json_node_get_int(node);
    return TRUE;
  }
  if(type == G_TYPE_DOUBLE)
  {
    double d = json_node_get_double(node);
    if(!isfinite(d)) return FALSE;
    if(d != trunc(d)) return FALSE;
    if(d < (double)G_MININT64 || d > (double)G_MAXINT64) return FALSE;
    *out = (gint64)d;
    return TRUE;
  }
  return FALSE;
}

static gboolean _require_string(JsonObject *params, const char *key, const char **out,
                                dt_remote_error_t **err)
{
  if(!params || !json_object_has_member(params, key))
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("missing required parameter '%s'"), key);
    return FALSE;
  }
  JsonNode *node = json_object_get_member(params, key);
  if(!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter '%s' must be a string"), key);
    return FALSE;
  }
  const char *s = json_node_get_string(node);
  if(strlen(s) > DT_REMOTE_PROTOCOL_MAX_STRING_LEN)
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter '%s' is too long"), key);
    return FALSE;
  }
  *out = s;
  return TRUE;
}

static gboolean _require_int(JsonObject *params, const char *key, gint64 *out,
                             dt_remote_error_t **err)
{
  if(!params || !json_object_has_member(params, key))
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("missing required parameter '%s'"), key);
    return FALSE;
  }
  JsonNode *node = json_object_get_member(params, key);
  if(!_json_node_get_integer(node, out))
  {
    if(err)
      *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter '%s' must be a finite integer"), key);
    return FALSE;
  }
  return TRUE;
}

static gboolean _optional_int_default(JsonObject *params, const char *key, gint64 def, gint64 *out,
                                      dt_remote_error_t **err)
{
  if(!params || !json_object_has_member(params, key))
  {
    *out = def;
    return TRUE;
  }
  JsonNode *node = json_object_get_member(params, key);
  if(!_json_node_get_integer(node, out))
  {
    if(err)
      *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter '%s' must be a finite integer"), key);
    return FALSE;
  }
  return TRUE;
}

typedef struct _known_keys_ctx_t
{
  const char *const *allowed;
  dt_remote_error_t *err;
} _known_keys_ctx_t;

static void _check_known_keys_cb(JsonObject *object, const gchar *member_name, JsonNode *member_node,
                                 gpointer user_data)
{
  (void)object;
  (void)member_node;
  _known_keys_ctx_t *ctx = user_data;
  if(ctx->err) return;  // already recorded the first unknown key; keep scanning cheaply

  for(const char *const *k = ctx->allowed; *k; k++)
    if(g_str_equal(*k, member_name)) return;

  ctx->err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("unknown parameter '%s'"), member_name);
}

// Strict-params rule: every key in `params` must be in `allowed`
// (NULL-terminated). `params == NULL` (no params given, or params was not
// even an object) trivially passes -- there are no keys to reject.
static gboolean _check_known_keys(JsonObject *params, const char *const *allowed,
                                  dt_remote_error_t **err)
{
  if(!params) return TRUE;

  _known_keys_ctx_t ctx = { .allowed = allowed, .err = NULL };
  json_object_foreach_member(params, _check_known_keys_cb, &ctx);
  if(ctx.err)
  {
    if(err) *err = ctx.err;
    else dt_remote_error_free(ctx.err);
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* neutral-value / field -> JSON                                           */
/* ---------------------------------------------------------------------- */

static JsonNode *_value_to_json(const dt_remote_value_t *v)
{
  JsonNode *node = json_node_new(JSON_NODE_VALUE);
  switch(v->type)
  {
    case DT_REMOTE_VALUE_FLOAT: json_node_set_double(node, v->v.f); break;
    case DT_REMOTE_VALUE_INT: json_node_set_int(node, v->v.i); break;
    case DT_REMOTE_VALUE_BOOL: json_node_set_boolean(node, v->v.b); break;
    case DT_REMOTE_VALUE_ENUM: json_node_set_string(node, v->v.e.name ? v->v.e.name : ""); break;
    default: json_node_set_string(node, "");  // unreachable; keep the builder well-formed
  }
  return node;
}

static JsonNode *_field_to_json(const dt_remote_field_t *f)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, f->name ? f->name : "");
  json_builder_set_member_name(b, "description");
  json_builder_add_string_value(b, f->description ? f->description : "");
  json_builder_set_member_name(b, "type");
  json_builder_add_string_value(b, f->type_name ? f->type_name : "opaque");

  if(f->has_range)
  {
    json_builder_set_member_name(b, "minimum");
    json_builder_add_double_value(b, f->minimum);
    json_builder_set_member_name(b, "maximum");
    json_builder_add_double_value(b, f->maximum);
  }
  if(f->has_default)
  {
    json_builder_set_member_name(b, "default");
    json_builder_add_value(b, _value_to_json(&f->default_value));
  }

  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, f->writable);

  if(f->represented_by)
  {
    json_builder_set_member_name(b, "represented_by");
    json_builder_begin_array(b);
    for(guint i = 0; i < f->represented_by->len; i++)
      json_builder_add_string_value(b, g_ptr_array_index(f->represented_by, i));
    json_builder_end_array(b);
  }

  if(f->enum_values)
  {
    json_builder_set_member_name(b, "enum_values");
    json_builder_begin_array(b);
    for(guint i = 0; i < f->enum_values->len; i++)
    {
      dt_remote_enum_value_t *e = g_ptr_array_index(f->enum_values, i);
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "name");
      json_builder_add_string_value(b, e->name ? e->name : "");
      json_builder_set_member_name(b, "value");
      json_builder_add_int_value(b, e->value);
      json_builder_set_member_name(b, "description");
      json_builder_add_string_value(b, e->description ? e->description : "");
      json_builder_end_object(b);
    }
    json_builder_end_array(b);
  }

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

/* ---------------------------------------------------------------------- */
/* semantic curve schema/value -> JSON (milestone 2; wire shapes are the   */
/* curve-classes design doc's SS Schema response / SS Value response)      */
/* ---------------------------------------------------------------------- */

static const char *_interpolation_name(dt_remote_curve_interpolation_t interpolation)
{
  switch(interpolation)
  {
    case DT_REMOTE_CURVE_CUBIC_SPLINE: return "CUBIC_SPLINE";
    case DT_REMOTE_CURVE_CATMULL_ROM: return "CATMULL_ROM";
    case DT_REMOTE_CURVE_MONOTONE_HERMITE: return "MONOTONE_HERMITE";
    default: return "";  // unreachable; keep the builder well-formed
  }
}

// NULL for DT_REMOTE_SPACING_NONE -- the spacing members are then omitted.
static const char *_spacing_comparison_name(dt_remote_spacing_rule_t rule)
{
  switch(rule)
  {
    case DT_REMOTE_SPACING_AT_LEAST: return "at_least";
    case DT_REMOTE_SPACING_GREATER_THAN: return "greater_than";
    default: return NULL;
  }
}

static const char *_boundary_policy_name(dt_remote_curve_endpoint_policy_t policy)
{
  switch(policy)
  {
    case DT_REMOTE_CURVE_BOUNDARY_POINTS_REQUIRED: return "required";
    case DT_REMOTE_CURVE_BOUNDARY_POINTS_FIXED_IDENTITY: return "fixed_identity";
    case DT_REMOTE_CURVE_BOUNDARY_POINTS_OPTIONAL:
    default: return "optional";
  }
}

// A structured field/operator/enum condition triple serializes as
// {"field": ..., "equals"/"not_equals": ...}.
static void _condition_to_json(JsonBuilder *b, const char *member,
                               const dt_remote_parameter_condition_t *condition)
{
  if(!condition) return;
  json_builder_set_member_name(b, member);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "field");
  json_builder_add_string_value(b, condition->field ? condition->field : "");
  json_builder_set_member_name(b, condition->op == DT_REMOTE_PREDICATE_EQ ? "equals" : "not_equals");
  json_builder_add_string_value(b, condition->enum_name ? condition->enum_name : "");
  json_builder_end_object(b);
}

static void _curve_axis_to_json(JsonBuilder *b, const char *member,
                                const dt_remote_curve_axis_t *axis)
{
  json_builder_set_member_name(b, member);
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "minimum");
  json_builder_add_double_value(b, axis->minimum);
  json_builder_set_member_name(b, "maximum");
  json_builder_add_double_value(b, axis->maximum);
  if(axis->unit)
  {
    json_builder_set_member_name(b, "unit");
    json_builder_add_string_value(b, axis->unit);
  }
  json_builder_end_object(b);
}

static JsonNode *_curve_schema_to_json(const dt_remote_curve_schema_t *s)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, s->name ? s->name : "");
  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "curve");
  json_builder_set_member_name(b, "display_name");
  json_builder_add_string_value(b, s->display_name ? s->display_name : "");

  json_builder_set_member_name(b, "readable");
  json_builder_add_boolean_value(b, TRUE);
  // "writable: true means the server implements writes for the class in
  // some valid state" -- writable_when below carries the static condition.
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, s->writability != DT_REMOTE_WRITABLE_NEVER);
  _condition_to_json(b, "writable_when", s->writable_when);

  _curve_axis_to_json(b, "x", &s->x);
  _curve_axis_to_json(b, "y", &s->y);

  json_builder_set_member_name(b, "points");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "minimum");
  json_builder_add_int_value(b, s->minimum_points);
  json_builder_set_member_name(b, "maximum");
  json_builder_add_int_value(b, s->maximum_points);
  json_builder_set_member_name(b, "strict_x_order");
  json_builder_add_boolean_value(b, s->strict_x_order);
  const char *spacing = _spacing_comparison_name(s->adjacent_spacing_rule);
  if(spacing)
  {
    json_builder_set_member_name(b, "minimum_x_spacing");
    json_builder_add_double_value(b, s->minimum_x_spacing);
    json_builder_set_member_name(b, "minimum_x_spacing_comparison");
    json_builder_add_string_value(b, spacing);
  }
  json_builder_set_member_name(b, "boundary_point_policy");
  json_builder_add_string_value(b, _boundary_policy_name(s->boundary_point_policy));
  json_builder_end_object(b);

  json_builder_set_member_name(b, "interpolation_values");
  json_builder_begin_array(b);
  for(dt_remote_curve_interpolation_t i = DT_REMOTE_CURVE_CUBIC_SPLINE;
      i <= DT_REMOTE_CURVE_MONOTONE_HERMITE;
      i++)
    if(s->interpolation_mask & (1u << i)) json_builder_add_string_value(b, _interpolation_name(i));
  json_builder_end_array(b);

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

static JsonNode *_curve_value_to_json(const dt_remote_curve_value_t *v)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "curve");
  json_builder_set_member_name(b, "active");
  json_builder_add_boolean_value(b, v->active);
  json_builder_set_member_name(b, "effective");
  json_builder_add_boolean_value(b, v->effective);
  json_builder_set_member_name(b, "writable_now");
  json_builder_add_boolean_value(b, v->writable_now);

  json_builder_set_member_name(b, "points");
  json_builder_begin_array(b);
  for(guint i = 0; v->points && i < v->points->len; i++)
  {
    const dt_remote_curve_point_t *p = &g_array_index(v->points, dt_remote_curve_point_t, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "x");
    json_builder_add_double_value(b, p->x);
    json_builder_set_member_name(b, "y");
    json_builder_add_double_value(b, p->y);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);

  json_builder_set_member_name(b, "interpolation");
  json_builder_add_string_value(b, _interpolation_name(v->interpolation));

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

/* ---------------------------------------------------------------------- */
/* semantic vector schema/value -> JSON (milestone 4; wire shapes are the  */
/* vector-class design doc's SS Wire and MCP contract "Schema row"/"Value")*/
/* ---------------------------------------------------------------------- */

// Stable wire vocabulary for dt_remote_vector_subtype_t -- like curve
// interpolation names, these strings are part of the wire contract.
static const char *_vector_subtype_name(dt_remote_vector_subtype_t subtype)
{
  switch(subtype)
  {
    case DT_REMOTE_VECTOR_COLOR: return "color";
    case DT_REMOTE_VECTOR_LEVELS: return "levels";
    case DT_REMOTE_VECTOR_PLAIN:
    default: return "vector";
  }
}

static JsonNode *_vector_schema_to_json(const dt_remote_vector_schema_t *s)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, s->name ? s->name : "");
  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "vector");
  json_builder_set_member_name(b, "display_name");
  json_builder_add_string_value(b, s->display_name ? s->display_name : "");
  json_builder_set_member_name(b, "subtype");
  json_builder_add_string_value(b, _vector_subtype_name(s->subtype));
  if(s->subtype == DT_REMOTE_VECTOR_COLOR)
  {
    json_builder_set_member_name(b, "color_space");
    json_builder_add_string_value(b, s->color_space ? s->color_space : "");
  }

  json_builder_set_member_name(b, "readable");
  json_builder_add_boolean_value(b, TRUE);
  // Same convention as the curve schema serializer: "writable: true" means
  // the server implements writes for the class in some valid state;
  // writable_when below carries the static condition.
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, s->writability != DT_REMOTE_WRITABLE_NEVER);
  _condition_to_json(b, "writable_when", s->writable_when);

  json_builder_set_member_name(b, "components");
  json_builder_begin_array(b);
  for(guint i = 0; s->components && i < s->components->len; i++)
  {
    const dt_remote_vector_component_schema_t *c =
      &g_array_index(s->components, dt_remote_vector_component_schema_t, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, c->name ? c->name : "");
    json_builder_set_member_name(b, "minimum");
    json_builder_add_double_value(b, c->minimum);
    json_builder_set_member_name(b, "maximum");
    json_builder_add_double_value(b, c->maximum);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);

  if(s->subtype == DT_REMOTE_VECTOR_LEVELS)
  {
    json_builder_set_member_name(b, "ordering");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "rule");
    json_builder_add_string_value(b, "strictly_increasing");
    json_builder_set_member_name(b, "minimum_gap");
    json_builder_add_double_value(b, s->minimum_gap);
    json_builder_set_member_name(b, "minimum_gap_comparison");
    json_builder_add_string_value(b, "at_least");
    json_builder_end_object(b);
  }

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

static JsonNode *_vector_value_to_json(const dt_remote_vector_value_t *v)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "vector");
  json_builder_set_member_name(b, "active");
  json_builder_add_boolean_value(b, v->active);
  json_builder_set_member_name(b, "effective");
  json_builder_add_boolean_value(b, v->effective);
  json_builder_set_member_name(b, "writable_now");
  json_builder_add_boolean_value(b, v->writable_now);

  json_builder_set_member_name(b, "values");
  json_builder_begin_array(b);
  for(guint i = 0; v->values && i < v->values->len; i++)
    json_builder_add_double_value(b, g_array_index(v->values, double, i));
  json_builder_end_array(b);

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

/* ---------------------------------------------------------------------- */
/* semantic band schema/value -> JSON (milestone 5; wire shapes are the    */
/* bands-class design doc's SS Wire and MCP contract "Schema"/"Value")     */
/* ---------------------------------------------------------------------- */

// Stable wire vocabulary for dt_remote_band_x_policy_t -- like vector
// subtype names, these strings are part of the wire contract.
static const char *_band_x_policy_name(dt_remote_band_x_policy_t x_policy)
{
  switch(x_policy)
  {
    case DT_REMOTE_BAND_X_INTERIOR: return "interior";
    case DT_REMOTE_BAND_X_FIXED:
    default: return "fixed";
  }
}

static JsonNode *_band_schema_to_json(const dt_remote_band_schema_t *s)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "name");
  json_builder_add_string_value(b, s->name ? s->name : "");
  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "bands");
  json_builder_set_member_name(b, "display_name");
  json_builder_add_string_value(b, s->display_name ? s->display_name : "");

  json_builder_set_member_name(b, "readable");
  json_builder_add_boolean_value(b, TRUE);
  // Same convention as the curve/vector schema serializers: "writable: true"
  // means the server implements writes for the class in some valid state;
  // writable_when below carries the static condition.
  json_builder_set_member_name(b, "writable");
  json_builder_add_boolean_value(b, s->writability != DT_REMOTE_WRITABLE_NEVER);
  _condition_to_json(b, "writable_when", s->writable_when);

  json_builder_set_member_name(b, "count");
  json_builder_add_int_value(b, s->count);

  json_builder_set_member_name(b, "y_range");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "minimum");
  json_builder_add_double_value(b, s->y_minimum);
  json_builder_set_member_name(b, "maximum");
  json_builder_add_double_value(b, s->y_maximum);
  json_builder_end_object(b);

  json_builder_set_member_name(b, "x_policy");
  json_builder_add_string_value(b, _band_x_policy_name(s->x_policy));
  if(s->x_policy == DT_REMOTE_BAND_X_INTERIOR)
  {
    json_builder_set_member_name(b, "min_gap");
    json_builder_add_double_value(b, s->minimum_gap);
  }
  if(s->x_shared_with)
  {
    json_builder_set_member_name(b, "x_shared_with");
    json_builder_add_string_value(b, s->x_shared_with);
  }

  // Current stored x positions -- present only when the schema carries them
  // (dt_remote_band_list_schema() has no live params blob to read from, so
  // schemas it produces omit the member).
  if(s->x)
  {
    json_builder_set_member_name(b, "x");
    json_builder_begin_array(b);
    for(guint i = 0; i < s->x->len; i++)
      json_builder_add_double_value(b, g_array_index(s->x, double, i));
    json_builder_end_array(b);
  }

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

static JsonNode *_band_value_to_json(const dt_remote_band_value_t *v)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "class");
  json_builder_add_string_value(b, "bands");
  json_builder_set_member_name(b, "active");
  json_builder_add_boolean_value(b, v->active);
  json_builder_set_member_name(b, "effective");
  json_builder_add_boolean_value(b, v->effective);
  json_builder_set_member_name(b, "writable_now");
  json_builder_add_boolean_value(b, v->writable_now);

  json_builder_set_member_name(b, "y");
  json_builder_begin_array(b);
  for(guint i = 0; v->y && i < v->y->len; i++)
    json_builder_add_double_value(b, g_array_index(v->y, double, i));
  json_builder_end_array(b);

  json_builder_set_member_name(b, "x");
  json_builder_begin_array(b);
  for(guint i = 0; v->x && i < v->x->len; i++)
    json_builder_add_double_value(b, g_array_index(v->x, double, i));
  json_builder_end_array(b);

  json_builder_end_object(b);
  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

// The semantic_fields/semantic_values containers carry class-tagged
// wrappers (remote_parameters.h); serialization switches on the tag.

static JsonNode *_semantic_schema_to_json(const dt_remote_semantic_schema_t *w)
{
  switch(w->class_id)
  {
    case DT_REMOTE_PARAMETER_CURVE:
      return _curve_schema_to_json(w->u.curve);
    case DT_REMOTE_PARAMETER_VECTOR:
      return _vector_schema_to_json(w->u.vector);
    case DT_REMOTE_PARAMETER_BANDS:
      return _band_schema_to_json(w->u.bands);
    default:
      g_assert_not_reached();
      return NULL;
  }
}

static JsonNode *_semantic_value_to_json(const dt_remote_semantic_value_t *w)
{
  switch(w->class_id)
  {
    case DT_REMOTE_PARAMETER_CURVE:
      return _curve_value_to_json(w->u.curve);
    case DT_REMOTE_PARAMETER_VECTOR:
      return _vector_value_to_json(w->u.vector);
    case DT_REMOTE_PARAMETER_BANDS:
      return _band_value_to_json(w->u.bands);
    default:
      g_assert_not_reached();
      return NULL;
  }
}

/* ---------------------------------------------------------------------- */
/* method handlers                                                         */
/* ---------------------------------------------------------------------- */

static const char *const HELLO_KEYS[] = { "protocol_version", "token", "client", NULL };

static JsonNode *_handler_hello(JsonObject *params, dt_remote_session_t *session,
                                dt_remote_pending_t *pending)
{
  (void)session;  // auth/session semantics land in step 4
  (void)pending;  // not async

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, HELLO_KEYS, &err)) return _handler_fail(err);

  gint64 protocol_version = 0;
  if(!_require_int(params, "protocol_version", &protocol_version, &err)) return _handler_fail(err);
  if(protocol_version != DT_REMOTE_PROTOCOL_VERSION)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                    _("unsupported protocol_version %" G_GINT64_FORMAT),
                                    protocol_version));

  // token/client are validated (present, string, within the length cap)
  // but not otherwise interpreted here -- verifying the token and
  // recording the client happens once remote_server owns a real session
  // (step 4).
  const char *token = NULL;
  if(!_require_string(params, "token", &token, &err)) return _handler_fail(err);
  const char *client = NULL;
  if(!_require_string(params, "client", &client, &err)) return _handler_fail(err);
  (void)token;
  (void)client;

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "protocol_version");
  json_builder_add_int_value(b, DT_REMOTE_PROTOCOL_VERSION);
  json_builder_set_member_name(b, "darktable_version");
  json_builder_add_string_value(b, darktable_package_version);
  json_builder_set_member_name(b, "pid");
  json_builder_add_int_value(b, (gint64)getpid());
  json_builder_set_member_name(b, "capabilities");
  json_builder_begin_array(b);
  // Each named capability ("params", "instances", "history", "preview",
  // "scopes") is advertised in the same commit that gives its gating
  // method a real handler in the allowlist below. "params" = parameter
  // mutation (set_module_params, plan step 7). "instances" =
  // create_module_instance; "history" = get_history/undo (plan step 8).
  // "preview" = render_preview (plan step 9). "scopes" = compute_scopes
  // (plan step 10). "semantic_params"/"curve_params" (milestone 2) gate
  // the additive semantic_fields/semantic_values wire members -- no
  // protocol_version bump, per the milestone spec's resolved decision.
  // "vector_params" (milestone 4) gates vector-class semantic_fields/
  // semantic_values entries the same way, on top of "semantic_params";
  // "band_params" (milestone 5) does the same for bands-class entries.
  json_builder_add_string_value(b, "params");
  json_builder_add_string_value(b, "semantic_params");
  json_builder_add_string_value(b, "curve_params");
  json_builder_add_string_value(b, "vector_params");
  json_builder_add_string_value(b, "band_params");
  json_builder_add_string_value(b, "instances");
  json_builder_add_string_value(b, "history");
  json_builder_add_string_value(b, "preview");
  json_builder_add_string_value(b, "scopes");
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  return result;
}

// get_state takes no params (protocol reference matrix: no invalid_value
// row) -- ignore whatever was sent rather than validating it.
static JsonNode *_handler_get_state(JsonObject *params, dt_remote_session_t *session,
                                    dt_remote_pending_t *pending)
{
  (void)params;
  (void)session;
  (void)pending;

  dt_remote_state_t *state = NULL;
  dt_remote_error_t *err = NULL;
  if(!s_calls.get_state(&state, &err)) return _handler_fail(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "view");
  json_builder_add_string_value(b, state->view ? state->view : "");

  json_builder_set_member_name(b, "image");
  if(state->has_image)
  {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_int_value(b, state->image_id);
    json_builder_set_member_name(b, "filename");
    json_builder_add_string_value(b, state->image_filename ? state->image_filename : "");
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, state->width);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, state->height);
    json_builder_set_member_name(b, "exif");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "maker");
    json_builder_add_string_value(b, state->maker ? state->maker : "");
    json_builder_set_member_name(b, "model");
    json_builder_add_string_value(b, state->model ? state->model : "");
    json_builder_set_member_name(b, "lens");
    json_builder_add_string_value(b, state->lens ? state->lens : "");
    json_builder_set_member_name(b, "iso");
    json_builder_add_double_value(b, state->iso);
    json_builder_set_member_name(b, "aperture");
    json_builder_add_double_value(b, state->aperture);
    json_builder_set_member_name(b, "exposure_time");
    json_builder_add_double_value(b, state->exposure_time);
    json_builder_set_member_name(b, "focal_length");
    json_builder_add_double_value(b, state->focal_length);
    json_builder_end_object(b);
    json_builder_end_object(b);
  }
  else
  {
    json_builder_add_null_value(b);
  }

  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)state->revision);
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  dt_remote_state_free(state);
  return result;
}

// list_modules takes no params either (same matrix row as get_state).
static JsonNode *_handler_list_modules(JsonObject *params, dt_remote_session_t *session,
                                       dt_remote_pending_t *pending)
{
  (void)params;
  (void)session;
  (void)pending;

  GPtrArray *modules = NULL;
  dt_remote_error_t *err = NULL;
  if(!s_calls.list_modules(&modules, &err)) return _handler_fail(err);

  // list_modules doesn't carry its own revision (only dt_remote_state_t
  // does) -- fetch it from get_state, which reads the real tracker
  // (internals §5, control/remote_revision.h) and always succeeds, so
  // this is a genuine internal inconsistency if it ever fails.
  dt_remote_state_t *state = NULL;
  dt_remote_error_t *state_err = NULL;
  if(!s_calls.get_state(&state, &state_err))
  {
    dt_remote_error_free(state_err);
    g_ptr_array_unref(modules);
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL, _("could not read current revision")));
  }
  uint64_t revision = state->revision;
  dt_remote_state_free(state);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)revision);
  json_builder_set_member_name(b, "modules");
  json_builder_begin_array(b);
  for(guint i = 0; i < modules->len; i++)
  {
    dt_remote_module_t *m = g_ptr_array_index(modules, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "op");
    json_builder_add_string_value(b, m->op ? m->op : "");
    json_builder_set_member_name(b, "instance");
    json_builder_add_int_value(b, m->instance);
    json_builder_set_member_name(b, "instance_name");
    json_builder_add_string_value(b, m->instance_name ? m->instance_name : "");
    json_builder_set_member_name(b, "display_name");
    json_builder_add_string_value(b, m->display_name ? m->display_name : "");
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, m->enabled);
    json_builder_set_member_name(b, "deprecated");
    json_builder_add_boolean_value(b, m->deprecated);
    json_builder_set_member_name(b, "supports_multiple_instances");
    json_builder_add_boolean_value(b, m->supports_multiple_instances);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  g_ptr_array_unref(modules);
  return result;
}

static const char *const GET_MODULE_SCHEMA_KEYS[] = { "module", NULL };

static JsonNode *_handler_get_module_schema(JsonObject *params, dt_remote_session_t *session,
                                            dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, GET_MODULE_SCHEMA_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  dt_remote_module_schema_t *schema = NULL;
  if(!s_calls.get_module_schema(module, &schema, &err)) return _handler_fail(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, schema->op ? schema->op : "");
  json_builder_set_member_name(b, "display_name");
  json_builder_add_string_value(b, schema->display_name ? schema->display_name : "");
  json_builder_set_member_name(b, "params_version");
  json_builder_add_int_value(b, schema->params_version);
  json_builder_set_member_name(b, "deprecated");
  json_builder_add_boolean_value(b, schema->deprecated);
  json_builder_set_member_name(b, "supports_multiple_instances");
  json_builder_add_boolean_value(b, schema->supports_multiple_instances);
  json_builder_set_member_name(b, "fields");
  json_builder_begin_array(b);
  if(schema->fields)
    for(guint i = 0; i < schema->fields->len; i++)
      json_builder_add_value(b, _field_to_json(g_ptr_array_index(schema->fields, i)));
  json_builder_end_array(b);
  // optional, capability-gated ("semantic_params"/"curve_params"): ops
  // without registered semantic curves emit no member at all -- their
  // responses stay byte-identical to the pre-milestone-2 wire shape.
  if(schema->semantic_fields && schema->semantic_fields->len > 0)
  {
    json_builder_set_member_name(b, "semantic_fields");
    json_builder_begin_array(b);
    for(guint i = 0; i < schema->semantic_fields->len; i++)
      json_builder_add_value(b, _semantic_schema_to_json(g_ptr_array_index(schema->semantic_fields, i)));
    json_builder_end_array(b);
  }
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  dt_remote_module_schema_free(schema);
  return result;
}

static const char *const GET_MODULE_PARAMS_KEYS[] = { "module", "instance", NULL };

static JsonNode *_handler_get_module_params(JsonObject *params, dt_remote_session_t *session,
                                            dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, GET_MODULE_PARAMS_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  gint64 instance = 0;
  if(!_optional_int_default(params, "instance", 0, &instance, &err)) return _handler_fail(err);

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };

  GPtrArray *values = NULL;
  GHashTable *semantic = NULL;
  if(!s_calls.get_module_params(&ref, &values, &semantic, &err)) return _handler_fail(err);

  // dt_remote_get_module_params() returns scalar values plus the semantic
  // curve values (milestone 2) -- the wire response also needs
  // instance_name/enabled (list_modules) and revision (get_state). Both
  // come from the same live module list this call just walked, so a
  // missing match here means the two calls disagreed -- treat that as
  // internal, not as if the module vanished mid-air.
  GPtrArray *modules = NULL;
  dt_remote_error_t *list_err = NULL;
  if(!s_calls.list_modules(&modules, &list_err))
  {
    dt_remote_error_free(list_err);
    g_ptr_array_unref(values);
    if(semantic) g_hash_table_unref(semantic);
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL,
                                    _("could not resolve instance metadata for '%s'/%d"), module,
                                    (int)instance));
  }

  const dt_remote_module_t *found = NULL;
  for(guint i = 0; i < modules->len; i++)
  {
    dt_remote_module_t *m = g_ptr_array_index(modules, i);
    if(!g_strcmp0(m->op, module) && m->instance == (int)instance)
    {
      found = m;
      break;
    }
  }
  if(!found)
  {
    g_ptr_array_unref(modules);
    g_ptr_array_unref(values);
    if(semantic) g_hash_table_unref(semantic);
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL,
                                    _("module '%s' instance %d vanished between reads"), module,
                                    (int)instance));
  }

  dt_remote_state_t *state = NULL;
  dt_remote_error_t *state_err = NULL;
  if(!s_calls.get_state(&state, &state_err))
  {
    dt_remote_error_free(state_err);
    g_ptr_array_unref(modules);
    g_ptr_array_unref(values);
    if(semantic) g_hash_table_unref(semantic);
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL, _("could not read current revision")));
  }

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, module);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, instance);
  json_builder_set_member_name(b, "instance_name");
  json_builder_add_string_value(b, found->instance_name ? found->instance_name : "");
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, found->enabled);
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)state->revision);
  json_builder_set_member_name(b, "values");
  json_builder_begin_object(b);
  for(guint i = 0; i < values->len; i++)
  {
    dt_remote_patch_entry_t *e = g_ptr_array_index(values, i);
    json_builder_set_member_name(b, e->name);
    json_builder_add_value(b, _value_to_json(&e->value));
  }
  json_builder_end_object(b);
  // optional, capability-gated: like semantic_fields, ops without
  // registered semantic curves emit no member. Keys are emitted sorted so
  // repeated reads serialize identically (GHashTable iteration order is
  // arbitrary).
  if(semantic && g_hash_table_size(semantic) > 0)
  {
    json_builder_set_member_name(b, "semantic_values");
    json_builder_begin_object(b);
    GList *keys = g_list_sort(g_hash_table_get_keys(semantic), (GCompareFunc)g_strcmp0);
    for(GList *k = keys; k; k = k->next)
    {
      json_builder_set_member_name(b, k->data);
      json_builder_add_value(b, _semantic_value_to_json(g_hash_table_lookup(semantic, k->data)));
    }
    g_list_free(keys);
    json_builder_end_object(b);
  }
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  dt_remote_state_free(state);
  g_ptr_array_unref(modules);
  g_ptr_array_unref(values);
  if(semantic) g_hash_table_unref(semantic);
  return result;
}

// Converts one raw JSON value into a dt_remote_value_t matched to `field`'s
// declared type -- this is where "enum by stable name and integer
// representation" (plan step 7's test list) is resolved: a JSON string is
// looked up by name against field->enum_values, a JSON integer is looked
// up by value; either produces a fully-formed {value, name} pair before
// dt_remote_patch_apply()/dt_remote_value_validate_and_write() ever sees
// it (that pure layer only compares the numeric value, never the name --
// see remote_edit.c). `field` must be non-NULL (the caller looks it up
// by name first and reports DT_REMOTE_ERR_UNKNOWN_FIELD itself if absent).
static gboolean _json_value_to_remote_value(JsonNode *node, const dt_remote_field_t *field,
                                            dt_remote_value_t *out, dt_remote_error_t **err)
{
  if(!field->writable)
  {
    if(err)
      *err = _error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, _("field '%s' is not writable"), field->name);
    return FALSE;
  }

  if(!g_strcmp0(field->type_name, "float"))
  {
    if(!node || !JSON_NODE_HOLDS_VALUE(node)) goto bad_type;
    GType t = json_node_get_value_type(node);
    double d;
    if(t == G_TYPE_DOUBLE) d = json_node_get_double(node);
    else if(t == G_TYPE_INT64) d = (double)json_node_get_int(node);
    else goto bad_type;
    // non-finite numbers (e.g. a 1e400 literal overflowing to +Inf on
    // parse) are rejected here at the protocol boundary, per the protocol
    // reference -- the engine's range check would also catch them, but
    // the handler must not depend on that through the test seam.
    if(!isfinite(d))
    {
      if(err)
        *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                          _("value for field '%s' must be finite"), field->name);
      return FALSE;
    }
    out->type = DT_REMOTE_VALUE_FLOAT;
    out->v.f = d;
    return TRUE;
  }

  if(!g_strcmp0(field->type_name, "int") || !g_strcmp0(field->type_name, "uint"))
  {
    gint64 i;
    if(!node || !_json_node_get_integer(node, &i)) goto bad_type;
    out->type = DT_REMOTE_VALUE_INT;
    out->v.i = i;
    return TRUE;
  }

  if(!g_strcmp0(field->type_name, "bool"))
  {
    if(!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
      goto bad_type;
    out->type = DT_REMOTE_VALUE_BOOL;
    out->v.b = json_node_get_boolean(node);
    return TRUE;
  }

  if(!g_strcmp0(field->type_name, "enum"))
  {
    if(node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
    {
      const char *name = json_node_get_string(node);
      for(guint i = 0; field->enum_values && i < field->enum_values->len; i++)
      {
        dt_remote_enum_value_t *e = g_ptr_array_index(field->enum_values, i);
        if(!g_strcmp0(e->name, name))
        {
          out->type = DT_REMOTE_VALUE_ENUM;
          out->v.e.value = e->value;
          out->v.e.name = g_strdup(e->name);
          return TRUE;
        }
      }
      if(err)
        *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'%s' is not a member of enum field '%s'"),
                          name, field->name);
      return FALSE;
    }

    gint64 i;
    if(node && _json_node_get_integer(node, &i))
    {
      for(guint j = 0; field->enum_values && j < field->enum_values->len; j++)
      {
        dt_remote_enum_value_t *e = g_ptr_array_index(field->enum_values, j);
        if(e->value == (int)i)
        {
          out->type = DT_REMOTE_VALUE_ENUM;
          out->v.e.value = e->value;
          out->v.e.name = g_strdup(e->name);
          return TRUE;
        }
      }
      if(err)
        *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                          _("%" G_GINT64_FORMAT " is not a member of enum field '%s'"), i, field->name);
      return FALSE;
    }
    goto bad_type;
  }

  // array/string/struct/opaque: a known field, but the wrong shape for a
  // v1 scalar patch entry -- surfaced the same as writable:false.
  if(err)
    *err = _error_new(DT_REMOTE_ERR_UNSUPPORTED_FIELD, _("field '%s' is not a writable scalar field"),
                      field->name);
  return FALSE;

bad_type:
  if(err)
    *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("value type does not match field '%s'"), field->name);
  return FALSE;
}

// The inverse of _interpolation_name(): one wire interpolation name (curve
// design SS Serialization rules: stable uppercase ASCII identifiers) back to
// the neutral enum. FALSE for anything else.
static gboolean _interpolation_from_name(const char *name, dt_remote_curve_interpolation_t *out)
{
  if(!g_strcmp0(name, "CUBIC_SPLINE")) { *out = DT_REMOTE_CURVE_CUBIC_SPLINE; return TRUE; }
  if(!g_strcmp0(name, "CATMULL_ROM")) { *out = DT_REMOTE_CURVE_CATMULL_ROM; return TRUE; }
  if(!g_strcmp0(name, "MONOTONE_HERMITE")) { *out = DT_REMOTE_CURVE_MONOTONE_HERMITE; return TRUE; }
  return FALSE;
}

// A curve point coordinate: a JSON number (double or integer spelling), and
// finite after parse -- 1e400-style overflow to +/-Inf is rejected here
// rather than handed to the engine.
static gboolean _node_to_finite_double(JsonNode *node, double *out)
{
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
  const GType type = json_node_get_value_type(node);
  if(type != G_TYPE_DOUBLE && type != G_TYPE_INT64) return FALSE;
  const double value = json_node_get_double(node);
  if(!isfinite(value)) return FALSE;
  *out = value;
  return TRUE;
}

// Flat pre-engine cap on points per semantic_values entry: request-size
// hygiene only -- the per-descriptor maximum_points bound (20 for rgbcurve)
// is the engine validator's business.
#define DT_REMOTE_CURVE_WIRE_POINT_CAP 64

// Flat pre-engine cap on components per vector semantic_values entry
// (milestone 4 design SS Protocol and engine changes item 2): request-size
// hygiene only -- the engine resolves semantic IDs against the vector
// registry and rejects component-count mismatches against the per-name
// descriptor.
#define DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP 8

// Flat pre-engine cap on samples per bands semantic_values entry (milestone
// 5 design): request-size hygiene only, mirroring the vector component cap
// above -- the engine resolves semantic IDs against the bands registry and
// rejects sample-count mismatches against the per-name descriptor.
#define DT_REMOTE_BAND_WIRE_SAMPLE_CAP 8

// Flat pre-engine cap on named components per quantity semantic_values
// entry (milestone 6 design): request-size hygiene only, mirroring the
// vector component cap and the bands sample cap above -- the engine
// resolves semantic IDs against the quantity registry and rejects
// component-name-set mismatches against the per-name descriptor.
#define DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP 8

// Parses the optional "semantic_values" request member into a GPtrArray of
// dt_remote_semantic_patch_t (curve design SS Mutation request /
// SS Serialization rules, extended by the milestone 4 vector-class design,
// the milestone 5 bands-class design, and the milestone 6 quantity-class
// design): entries are objects with exactly {class, points[, interpolation]}
// for class "curve" (points are objects with exactly {x, y}, both finite
// numbers), exactly {class, values} for class "vector" (values is a
// non-empty array of at most DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP finite
// numbers), exactly {class, y[, x]} for class "bands" (y is a non-empty
// array of at most DT_REMOTE_BAND_WIRE_SAMPLE_CAP finite numbers; x, if
// present, is a finite numeric array of the same length as y), or exactly
// {class, values} for class "quantity" (values is an object with 1..
// DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP members, keyed by component name,
// each a finite number; member order is preserved into the patch). Values/
// components/samples are carried as doubles end-to-end -- narrowing to
// float only happens once the engine writes into the params blob, so a
// range check never sees a value that has already rounded into range.
// Duplicate semantic IDs cannot survive to this point -- JsonObject keys
// are already unique. Absent member: *out stays NULL, returns TRUE. Any
// violation: FALSE with *err set and *out NULL -- the engine is never
// reached with a partially parsed patch.
static gboolean _parse_semantic_values(JsonObject *params, GPtrArray **out, dt_remote_error_t **err)
{
  *out = NULL;
  if(!params || !json_object_has_member(params, "semantic_values")) return TRUE;

  JsonNode *node = json_object_get_member(params, "semantic_values");
  if(!node || !JSON_NODE_HOLDS_OBJECT(node))
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'semantic_values' must be an object"));
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(node);
  GList *keys = json_object_get_members(obj);
  if(!keys)
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'semantic_values' must be non-empty"));
    return FALSE;
  }

  GPtrArray *patches = g_ptr_array_new_with_free_func(dt_remote_semantic_patch_free);
  dt_remote_error_t *entry_err = NULL;
  for(GList *k = keys; k && !entry_err; k = k->next)
  {
    const char *name = k->data;
    JsonNode *entry_node = json_object_get_member(obj, name);
    if(!entry_node || !JSON_NODE_HOLDS_OBJECT(entry_node))
    {
      entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                             _("semantic value '%s' must be an object"), name);
      break;
    }
    JsonObject *entry = json_node_get_object(entry_node);

    // class is required in mutation values -- curve design SS Serialization
    // rules ("to prevent future ambiguous shapes"); "curve", "vector",
    // "bands", and "quantity" are the v1 writable classes.
    JsonNode *class_node = json_object_get_member(entry, "class");
    const char *class_name =
      (class_node && JSON_NODE_HOLDS_VALUE(class_node)
       && json_node_get_value_type(class_node) == G_TYPE_STRING)
        ? json_node_get_string(class_node) : NULL;
    if(!class_name)
    {
      entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                             _("semantic value '%s' requires a string 'class' member"), name);
      break;
    }

    const gboolean is_curve = !g_strcmp0(class_name, "curve");
    const gboolean is_vector = !g_strcmp0(class_name, "vector");
    const gboolean is_bands = !g_strcmp0(class_name, "bands");
    const gboolean is_quantity = !g_strcmp0(class_name, "quantity");

    // strict member set, per class: exactly class + points, optionally
    // interpolation, for curve; exactly class + values for vector; exactly
    // class + y, optionally x, for bands; exactly class + values for
    // quantity (note: "values" here is an object keyed by component name,
    // not the vector class's array -- the two classes share the member
    // name but not its shape). An unrecognized class skips this check
    // entirely -- it is rejected below regardless of which other members
    // it carries.
    if(is_curve || is_vector || is_bands || is_quantity)
    {
      GList *members = json_object_get_members(entry);
      for(GList *m = members; m && !entry_err; m = m->next)
      {
        const char *member = m->data;
        const gboolean allowed =
          !g_strcmp0(member, "class")
          || (is_curve && (!g_strcmp0(member, "points") || !g_strcmp0(member, "interpolation")))
          || (is_vector && !g_strcmp0(member, "values"))
          || (is_bands && (!g_strcmp0(member, "y") || !g_strcmp0(member, "x")))
          || (is_quantity && !g_strcmp0(member, "values"));
        if(!allowed)
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("unknown member '%s' in semantic value '%s'"), member, name);
      }
      g_list_free(members);
      if(entry_err) break;
    }

    if(is_curve)
    {
      JsonNode *points_node = json_object_get_member(entry, "points");
      if(!points_node || !JSON_NODE_HOLDS_ARRAY(points_node))
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' requires a 'points' array"), name);
        break;
      }
      JsonArray *points_array = json_node_get_array(points_node);
      const guint n_points = json_array_get_length(points_array);
      if(n_points > DT_REMOTE_CURVE_WIRE_POINT_CAP)
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' has %u points; the request limit is %d"),
                               name, n_points, DT_REMOTE_CURVE_WIRE_POINT_CAP);
        break;
      }

      GArray *points = g_array_sized_new(FALSE, FALSE, sizeof(dt_remote_curve_point_t), n_points);
      for(guint i = 0; i < n_points && !entry_err; i++)
      {
        JsonNode *point_node = json_array_get_element(points_array, i);
        JsonObject *point =
          (point_node && JSON_NODE_HOLDS_OBJECT(point_node)) ? json_node_get_object(point_node) : NULL;
        dt_remote_curve_point_t p = { 0 };
        // curve design SS Serialization rules: "curve point objects require
        // exactly x and y" -- a size-2 object with both members present has
        // no room for anything else.
        const gboolean ok = point && json_object_get_size(point) == 2
                            && _node_to_finite_double(json_object_get_member(point, "x"), &p.x)
                            && _node_to_finite_double(json_object_get_member(point, "y"), &p.y);
        if(!ok)
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("point %u of semantic value '%s' must be an object with exactly "
                                   "finite numeric 'x' and 'y'"), i, name);
        else
          g_array_append_val(points, p);
      }
      if(entry_err)
      {
        g_array_unref(points);
        break;
      }

      gboolean has_interpolation = FALSE;
      dt_remote_curve_interpolation_t interpolation = DT_REMOTE_CURVE_CUBIC_SPLINE;
      if(json_object_has_member(entry, "interpolation"))
      {
        JsonNode *interp_node = json_object_get_member(entry, "interpolation");
        const char *interp_name =
          (interp_node && JSON_NODE_HOLDS_VALUE(interp_node)
           && json_node_get_value_type(interp_node) == G_TYPE_STRING)
            ? json_node_get_string(interp_node) : NULL;
        if(!interp_name || !_interpolation_from_name(interp_name, &interpolation))
        {
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("unknown interpolation for semantic value '%s'"), name);
          g_array_unref(points);
          break;
        }
        has_interpolation = TRUE;
      }

      dt_remote_semantic_patch_t *semantic = g_malloc0(sizeof(dt_remote_semantic_patch_t));
      semantic->class_id = DT_REMOTE_PARAMETER_CURVE;
      semantic->value.curve.name = g_strdup(name);
      semantic->value.curve.points = points;
      semantic->value.curve.has_interpolation = has_interpolation;
      semantic->value.curve.interpolation = interpolation;
      g_ptr_array_add(patches, semantic);
    }
    else if(is_vector)
    {
      JsonNode *values_node = json_object_get_member(entry, "values");
      if(!values_node || !JSON_NODE_HOLDS_ARRAY(values_node))
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' requires a 'values' array"), name);
        break;
      }
      JsonArray *arr = json_node_get_array(values_node);
      const guint n = json_array_get_length(arr);
      if(n == 0 || n > DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP)
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' has %u components; the request limit is %d"),
                               name, n, DT_REMOTE_VECTOR_WIRE_COMPONENT_CAP);
        break;
      }
      GArray *values = g_array_sized_new(FALSE, FALSE, sizeof(double), n);
      for(guint i = 0; i < n && !entry_err; i++)
      {
        double v = 0.0;
        if(!_node_to_finite_double(json_array_get_element(arr, i), &v))
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("component %u of semantic value '%s' must be a finite number"),
                                 i, name);
        else
          g_array_append_val(values, v);
      }
      if(entry_err)
      {
        g_array_unref(values);
        break;
      }

      dt_remote_semantic_patch_t *semantic = g_malloc0(sizeof(dt_remote_semantic_patch_t));
      semantic->class_id = DT_REMOTE_PARAMETER_VECTOR;
      semantic->value.vector.name = g_strdup(name);
      semantic->value.vector.values = values;
      g_ptr_array_add(patches, semantic);
    }
    else if(is_bands)
    {
      JsonNode *y_node = json_object_get_member(entry, "y");
      if(!y_node || !JSON_NODE_HOLDS_ARRAY(y_node))
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' requires a 'y' array"), name);
        break;
      }
      JsonArray *y_arr = json_node_get_array(y_node);
      const guint n_y = json_array_get_length(y_arr);
      if(n_y == 0 || n_y > DT_REMOTE_BAND_WIRE_SAMPLE_CAP)
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' has %u samples; the request limit is %d"),
                               name, n_y, DT_REMOTE_BAND_WIRE_SAMPLE_CAP);
        break;
      }
      GArray *y = g_array_sized_new(FALSE, FALSE, sizeof(double), n_y);
      for(guint i = 0; i < n_y && !entry_err; i++)
      {
        double v = 0.0;
        if(!_node_to_finite_double(json_array_get_element(y_arr, i), &v))
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("sample %u of semantic value '%s' must be a finite number"),
                                 i, name);
        else
          g_array_append_val(y, v);
      }
      if(entry_err)
      {
        g_array_unref(y);
        break;
      }

      // "x", if present, is the sample abscissae: same element rules as
      // "y", and its length must match "y"'s -- the engine pairs x[i] with
      // y[i] positionally, so a length mismatch is a wire-level shape
      // error, not a validator concern.
      GArray *x = NULL;
      if(json_object_has_member(entry, "x"))
      {
        JsonNode *x_node = json_object_get_member(entry, "x");
        if(!x_node || !JSON_NODE_HOLDS_ARRAY(x_node))
        {
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("semantic value '%s' member 'x' must be an array"), name);
          g_array_unref(y);
          break;
        }
        JsonArray *x_arr = json_node_get_array(x_node);
        const guint n_x = json_array_get_length(x_arr);
        if(n_x != n_y)
        {
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("semantic value '%s' has %u 'x' samples but %u 'y' samples; "
                                   "the lengths must match"), name, n_x, n_y);
          g_array_unref(y);
          break;
        }
        x = g_array_sized_new(FALSE, FALSE, sizeof(double), n_x);
        for(guint i = 0; i < n_x && !entry_err; i++)
        {
          double v = 0.0;
          if(!_node_to_finite_double(json_array_get_element(x_arr, i), &v))
            entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                   _("x sample %u of semantic value '%s' must be a finite number"),
                                   i, name);
          else
            g_array_append_val(x, v);
        }
        if(entry_err)
        {
          g_array_unref(x);
          g_array_unref(y);
          break;
        }
      }

      dt_remote_semantic_patch_t *semantic = g_malloc0(sizeof(dt_remote_semantic_patch_t));
      semantic->class_id = DT_REMOTE_PARAMETER_BANDS;
      semantic->value.bands.name = g_strdup(name);
      semantic->value.bands.y = y;
      semantic->value.bands.x = x;
      g_ptr_array_add(patches, semantic);
    }
    else if(is_quantity)
    {
      JsonNode *values_node = json_object_get_member(entry, "values");
      if(!values_node || !JSON_NODE_HOLDS_OBJECT(values_node))
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' requires a 'values' object"), name);
        break;
      }
      JsonObject *values_obj = json_node_get_object(values_node);
      GList *component_keys = json_object_get_members(values_obj);
      const guint n_components = g_list_length(component_keys);
      if(n_components == 0 || n_components > DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP)
      {
        entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                               _("semantic value '%s' has %u components; the request limit is %d"),
                               name, n_components, DT_REMOTE_QUANTITY_WIRE_COMPONENT_CAP);
        g_list_free(component_keys);
        break;
      }

      GPtrArray *values =
        g_ptr_array_new_with_free_func(dt_remote_quantity_component_value_free);
      for(GList *c = component_keys; c && !entry_err; c = c->next)
      {
        const char *component_name = c->data;
        double v = 0.0;
        if(!_node_to_finite_double(json_object_get_member(values_obj, component_name), &v))
        {
          entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                 _("component '%s' of semantic value '%s' must be a finite number"),
                                 component_name, name);
          break;
        }
        dt_remote_quantity_component_value_t *component =
          g_new0(dt_remote_quantity_component_value_t, 1);
        component->name = g_strdup(component_name);
        component->value = v;
        g_ptr_array_add(values, component);
      }
      g_list_free(component_keys);
      if(entry_err)
      {
        g_ptr_array_unref(values);
        break;
      }

      dt_remote_semantic_patch_t *semantic = g_malloc0(sizeof(dt_remote_semantic_patch_t));
      semantic->class_id = DT_REMOTE_PARAMETER_QUANTITY;
      semantic->value.quantity.name = g_strdup(name);
      semantic->value.quantity.values = values;
      g_ptr_array_add(patches, semantic);
    }
    else
    {
      entry_err = _error_new(DT_REMOTE_ERR_INVALID_VALUE,
                             _("unsupported class '%s' for semantic value '%s'"), class_name, name);
      break;
    }
  }
  g_list_free(keys);

  if(entry_err)
  {
    g_ptr_array_unref(patches);
    if(err) *err = entry_err;
    else dt_remote_error_free(entry_err);
    return FALSE;
  }

  *out = patches;
  return TRUE;
}

static const char *const SET_MODULE_PARAMS_KEYS[] =
  { "module", "instance", "values", "semantic_values", "expected_revision", "enable", NULL };

static JsonNode *_handler_set_module_params(JsonObject *params, dt_remote_session_t *session,
                                            dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, SET_MODULE_PARAMS_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  gint64 instance = 0;
  if(!_optional_int_default(params, "instance", 0, &instance, &err)) return _handler_fail(err);

  // "values must be non-empty, contain no duplicate or unknown fields"
  // (wire contract) -- duplicates cannot occur once parsed into a
  // JsonObject (keys are already unique); unknown/non-empty are checked
  // here and per-entry below.
  if(!params || !json_object_has_member(params, "values"))
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("missing required parameter 'values'")));
  JsonNode *values_node = json_object_get_member(params, "values");
  if(!values_node || !JSON_NODE_HOLDS_OBJECT(values_node))
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'values' must be an object")));
  JsonObject *values_obj = json_node_get_object(values_node);

  // Optional semantic_values (milestone 2 Task 9): fully validated before
  // any engine work -- and before the "values must be non-empty" rule,
  // which relaxes to "at least one of values/semantic_values" when a
  // semantic patch is present (curve design SS Mutation request). Requests
  // without the member keep today's behavior exactly.
  GPtrArray *semantic_patches = NULL;
  if(!_parse_semantic_values(params, &semantic_patches, &err)) return _handler_fail(err);

  GList *value_keys = json_object_get_members(values_obj);
  if(!value_keys && !semantic_patches)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'values' must be non-empty")));

  gboolean have_expected_revision = FALSE;
  gint64 expected_revision = 0;
  if(json_object_has_member(params, "expected_revision"))
  {
    if(!_require_int(params, "expected_revision", &expected_revision, &err))
    {
      g_list_free(value_keys);
      if(semantic_patches) g_ptr_array_unref(semantic_patches);
      return _handler_fail(err);
    }
    if(expected_revision < 0)
    {
      g_list_free(value_keys);
      if(semantic_patches) g_ptr_array_unref(semantic_patches);
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("'expected_revision' must not be negative")));
    }
    have_expected_revision = TRUE;
  }

  gboolean have_enable = FALSE;
  gboolean enable_value = FALSE;
  if(json_object_has_member(params, "enable"))
  {
    JsonNode *enable_node = json_object_get_member(params, "enable");
    if(!enable_node || !JSON_NODE_HOLDS_VALUE(enable_node)
       || json_node_get_value_type(enable_node) != G_TYPE_BOOLEAN)
    {
      g_list_free(value_keys);
      if(semantic_patches) g_ptr_array_unref(semantic_patches);
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'enable' must be a boolean")));
    }
    enable_value = json_node_get_boolean(enable_node);
    have_enable = TRUE;
  }

  // Resolve field types via the schema -- needed to interpret each raw
  // JSON value (e.g. distinguish an enum's stable-name/int form from a
  // plain int field). Scalar conversion deliberately uses the primitive
  // schema so unrelated semantic registry failures cannot block mutation.
  dt_remote_module_schema_t *schema = NULL;
  if(!s_calls.get_module_primitive_schema(module, &schema, &err))
  {
    g_list_free(value_keys);
    if(semantic_patches) g_ptr_array_unref(semantic_patches);
    return _handler_fail(err);
  }

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
  patch.semantic_values = semantic_patches;  // may be NULL
  patch.has_enable = have_enable;
  patch.enable = enable_value;

  dt_remote_error_t *convert_err = NULL;
  for(GList *k = value_keys; k; k = k->next)
  {
    const char *name = k->data;
    JsonNode *node = json_object_get_member(values_obj, name);

    dt_remote_field_t *field = NULL;
    if(schema->fields)
      for(guint i = 0; i < schema->fields->len; i++)
      {
        dt_remote_field_t *f = g_ptr_array_index(schema->fields, i);
        if(!g_strcmp0(f->name, name)) { field = f; break; }
      }

    if(!field)
    {
      convert_err = _error_new(DT_REMOTE_ERR_UNKNOWN_FIELD, _("unknown field '%s'"), name);
      break;
    }

    dt_remote_value_t value;
    if(!_json_value_to_remote_value(node, field, &value, &convert_err)) break;

    dt_remote_patch_entry_t *entry = g_malloc0(sizeof(dt_remote_patch_entry_t));
    entry->name = g_strdup(name);
    entry->value = value;
    g_ptr_array_add(patch.scalar_values, entry);
  }
  g_list_free(value_keys);
  dt_remote_module_schema_free(schema);

  if(convert_err)
  {
    g_ptr_array_unref(patch.scalar_values);
    if(patch.semantic_values) g_ptr_array_unref(patch.semantic_values);
    return _handler_fail(convert_err);
  }

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };
  const uint64_t expected_u64 = (uint64_t)expected_revision;

  dt_remote_mutation_result_t *result = NULL;
  gboolean ok = s_calls.set_module_params(&ref, &patch, have_expected_revision ? &expected_u64 : NULL,
                                          &result, &err);
  g_ptr_array_unref(patch.scalar_values);
  if(patch.semantic_values) g_ptr_array_unref(patch.semantic_values);

  if(!ok) return _handler_fail(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, result->op ? result->op : module);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, result->instance);
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, result->enabled);
  json_builder_set_member_name(b, "values");
  json_builder_begin_object(b);
  if(result->values)
    for(guint i = 0; i < result->values->len; i++)
    {
      dt_remote_patch_entry_t *e = g_ptr_array_index(result->values, i);
      json_builder_set_member_name(b, e->name);
      json_builder_add_value(b, _value_to_json(&e->value));
    }
  json_builder_end_object(b);
  // Read-back for every semantic entry the patch wrote (curve design
  // SS Mutation request); requests without semantic_values emit no member,
  // byte-identical to today. Keys sorted, like get_module_params.
  if(result->semantic_values && g_hash_table_size(result->semantic_values) > 0)
  {
    json_builder_set_member_name(b, "semantic_values");
    json_builder_begin_object(b);
    GList *keys = g_list_sort(g_hash_table_get_keys(result->semantic_values), (GCompareFunc)g_strcmp0);
    for(GList *k = keys; k; k = k->next)
    {
      json_builder_set_member_name(b, k->data);
      json_builder_add_value(b, _semantic_value_to_json(g_hash_table_lookup(result->semantic_values, k->data)));
    }
    g_list_free(keys);
    json_builder_end_object(b);
  }
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)result->revision);
  json_builder_end_object(b);

  JsonNode *result_node = json_builder_get_root(b);
  g_object_unref(b);
  dt_remote_mutation_result_free(result);
  return result_node;
}

// Shared optional expected_revision parse used by every step-8 mutation
// (and set_module_params inlines the same rule): absent -> have=FALSE;
// present -> must be a finite, non-negative integer, else invalid_value.
static gboolean _parse_expected_revision(JsonObject *params, gboolean *have, gint64 *value,
                                         dt_remote_error_t **err)
{
  *have = FALSE;
  *value = 0;
  if(!params || !json_object_has_member(params, "expected_revision")) return TRUE;
  if(!_require_int(params, "expected_revision", value, err)) return FALSE;
  if(*value < 0)
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'expected_revision' must not be negative"));
    return FALSE;
  }
  *have = TRUE;
  return TRUE;
}

// Serializes a mutation result (op/instance/enabled/revision, plus
// instance_name and/or values only when requested) -- the wire shapes for
// set_module_enabled, reset_module, and create_module_instance differ only
// in which of those two optional members appear.
static JsonNode *_mutation_result_to_json(const dt_remote_mutation_result_t *result,
                                           const char *fallback_module,
                                           gboolean with_instance_name, gboolean with_values)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "module");
  json_builder_add_string_value(b, result->op ? result->op : fallback_module);
  json_builder_set_member_name(b, "instance");
  json_builder_add_int_value(b, result->instance);
  if(with_instance_name)
  {
    json_builder_set_member_name(b, "instance_name");
    json_builder_add_string_value(b, result->instance_name ? result->instance_name : "");
  }
  json_builder_set_member_name(b, "enabled");
  json_builder_add_boolean_value(b, result->enabled);
  if(with_values)
  {
    json_builder_set_member_name(b, "values");
    json_builder_begin_object(b);
    if(result->values)
      for(guint i = 0; i < result->values->len; i++)
      {
        dt_remote_patch_entry_t *e = g_ptr_array_index(result->values, i);
        json_builder_set_member_name(b, e->name);
        json_builder_add_value(b, _value_to_json(&e->value));
      }
    json_builder_end_object(b);
  }
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)result->revision);
  json_builder_end_object(b);

  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

static const char *const SET_MODULE_ENABLED_KEYS[] =
  { "module", "instance", "enabled", "expected_revision", NULL };

static JsonNode *_handler_set_module_enabled(JsonObject *params, dt_remote_session_t *session,
                                             dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, SET_MODULE_ENABLED_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  gint64 instance = 0;
  if(!_optional_int_default(params, "instance", 0, &instance, &err)) return _handler_fail(err);

  // `enabled` is required and explicit -- enabling is never implicit.
  if(!params || !json_object_has_member(params, "enabled"))
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("missing required parameter 'enabled'")));
  JsonNode *enabled_node = json_object_get_member(params, "enabled");
  if(!enabled_node || !JSON_NODE_HOLDS_VALUE(enabled_node)
     || json_node_get_value_type(enabled_node) != G_TYPE_BOOLEAN)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'enabled' must be a boolean")));
  const gboolean enabled = json_node_get_boolean(enabled_node);

  gboolean have_expected = FALSE;
  gint64 expected_revision = 0;
  if(!_parse_expected_revision(params, &have_expected, &expected_revision, &err))
    return _handler_fail(err);

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };
  const uint64_t expected_u64 = (uint64_t)expected_revision;

  dt_remote_mutation_result_t *result = NULL;
  if(!s_calls.set_module_enabled(&ref, enabled, have_expected ? &expected_u64 : NULL, &result, &err))
    return _handler_fail(err);

  JsonNode *node = _mutation_result_to_json(result, module, FALSE, FALSE);
  dt_remote_mutation_result_free(result);
  return node;
}

static const char *const RESET_MODULE_KEYS[] = { "module", "instance", "expected_revision", NULL };

static JsonNode *_handler_reset_module(JsonObject *params, dt_remote_session_t *session,
                                       dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, RESET_MODULE_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  gint64 instance = 0;
  if(!_optional_int_default(params, "instance", 0, &instance, &err)) return _handler_fail(err);

  gboolean have_expected = FALSE;
  gint64 expected_revision = 0;
  if(!_parse_expected_revision(params, &have_expected, &expected_revision, &err))
    return _handler_fail(err);

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };
  const uint64_t expected_u64 = (uint64_t)expected_revision;

  dt_remote_mutation_result_t *result = NULL;
  if(!s_calls.reset_module(&ref, have_expected ? &expected_u64 : NULL, &result, &err))
    return _handler_fail(err);

  // reset returns the post-reset values (not instance_name).
  JsonNode *node = _mutation_result_to_json(result, module, FALSE, TRUE);
  dt_remote_mutation_result_free(result);
  return node;
}

static const char *const CREATE_MODULE_INSTANCE_KEYS[] =
  { "module", "source_instance", "copy_params", "expected_revision", NULL };

static JsonNode *_handler_create_module_instance(JsonObject *params, dt_remote_session_t *session,
                                                 dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, CREATE_MODULE_INSTANCE_KEYS, &err)) return _handler_fail(err);

  const char *module = NULL;
  if(!_require_string(params, "module", &module, &err)) return _handler_fail(err);

  gint64 source_instance = 0;
  if(!_optional_int_default(params, "source_instance", 0, &source_instance, &err))
    return _handler_fail(err);

  gboolean copy_params = FALSE;
  if(json_object_has_member(params, "copy_params"))
  {
    JsonNode *cp_node = json_object_get_member(params, "copy_params");
    if(!cp_node || !JSON_NODE_HOLDS_VALUE(cp_node)
       || json_node_get_value_type(cp_node) != G_TYPE_BOOLEAN)
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("parameter 'copy_params' must be a boolean")));
    copy_params = json_node_get_boolean(cp_node);
  }

  gboolean have_expected = FALSE;
  gint64 expected_revision = 0;
  if(!_parse_expected_revision(params, &have_expected, &expected_revision, &err))
    return _handler_fail(err);

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)source_instance };
  const uint64_t expected_u64 = (uint64_t)expected_revision;

  dt_remote_mutation_result_t *result = NULL;
  if(!s_calls.create_module_instance(&ref, copy_params, have_expected ? &expected_u64 : NULL,
                                     &result, &err))
    return _handler_fail(err);

  // create returns instance_name (the new instance) but no values.
  JsonNode *node = _mutation_result_to_json(result, module, TRUE, FALSE);
  dt_remote_mutation_result_free(result);
  return node;
}

static const char *const GET_HISTORY_KEYS[] = { "limit", NULL };

static JsonNode *_handler_get_history(JsonObject *params, dt_remote_session_t *session,
                                      dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, GET_HISTORY_KEYS, &err)) return _handler_fail(err);

  // limit defaults to 20 and is clamped to the server's [1,100] window --
  // out-of-range integers are clamped, not rejected (per the reference's
  // "server cap 100"); only a non-integer/fractional limit is invalid_value.
  gint64 limit = 20;
  if(!_optional_int_default(params, "limit", 20, &limit, &err)) return _handler_fail(err);
  if(limit < 1) limit = 1;
  if(limit > 100) limit = 100;

  GPtrArray *items = NULL;
  uint64_t revision = 0;
  if(!s_calls.get_history((int)limit, &items, &revision, &err)) return _handler_fail(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)revision);
  json_builder_set_member_name(b, "items");
  json_builder_begin_array(b);
  for(guint i = 0; i < items->len; i++)
  {
    dt_remote_history_item_t *it = g_ptr_array_index(items, i);
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "seq");
    json_builder_add_int_value(b, it->seq);
    json_builder_set_member_name(b, "op");
    json_builder_add_string_value(b, it->op ? it->op : "");
    json_builder_set_member_name(b, "instance");
    json_builder_add_int_value(b, it->instance);
    json_builder_set_member_name(b, "display_name");
    json_builder_add_string_value(b, it->display_name ? it->display_name : "");
    json_builder_set_member_name(b, "instance_name");
    json_builder_add_string_value(b, it->instance_name ? it->instance_name : "");
    json_builder_set_member_name(b, "enabled");
    json_builder_add_boolean_value(b, it->enabled);
    json_builder_end_object(b);
  }
  json_builder_end_array(b);
  json_builder_end_object(b);

  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  g_ptr_array_unref(items);
  return node;
}

static const char *const UNDO_KEYS[] = { "expected_revision", NULL };

static JsonNode *_handler_undo(JsonObject *params, dt_remote_session_t *session,
                               dt_remote_pending_t *pending)
{
  (void)session;
  (void)pending;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, UNDO_KEYS, &err)) return _handler_fail(err);

  // expected_revision is REQUIRED for undo (compare-and-undo; no
  // unconditional form and no `steps` in v1).
  gint64 expected_revision = 0;
  if(!_require_int(params, "expected_revision", &expected_revision, &err)) return _handler_fail(err);
  if(expected_revision < 0)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                    _("'expected_revision' must not be negative")));

  uint64_t revision = 0;
  if(!s_calls.undo((uint64_t)expected_revision, &revision, &err)) return _handler_fail(err);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "revision");
  json_builder_add_int_value(b, (gint64)revision);
  json_builder_end_object(b);

  JsonNode *node = json_builder_get_root(b);
  g_object_unref(b);
  return node;
}

static const char *const RENDER_PREVIEW_KEYS[] = { "max_px", "quality", NULL };

// The first asynchronous handler: validates and clamps params, does the
// main-thread pre-work (history flush + revision capture, via the calls
// table), queues the background render (via the async table), and defers
// by returning NULL with no error set. Dispatch created `pending` before
// calling here (it knows the request id; this handler does not) and
// releases it again if this handler resolves synchronously.
static JsonNode *_handler_render_preview(JsonObject *params, dt_remote_session_t *session,
                                         dt_remote_pending_t *pending)
{
  (void)session;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, RENDER_PREVIEW_KEYS, &err)) return _handler_fail(err);

  // Out-of-range integers are clamped, not rejected -- the wire contract
  // says "max_px is clamped to [64, 2048]; quality to [50, 95]" (defaults
  // 1024/85); only a non-integer/fractional/non-finite value is
  // invalid_value.
  gint64 max_px = 1024;
  if(!_optional_int_default(params, "max_px", 1024, &max_px, &err)) return _handler_fail(err);
  if(max_px < 64) max_px = 64;
  if(max_px > 2048) max_px = 2048;

  gint64 quality = 85;
  if(!_optional_int_default(params, "quality", 85, &quality, &err)) return _handler_fail(err);
  if(quality < 50) quality = 50;
  if(quality > 95) quality = 95;

  // main-thread pre-work (internals §8, binding): flush live history to
  // the DB (the export re-loads it from there) and capture the revision
  // at that instant -- the preview is stamped with it. Also where
  // not_in_darkroom/no_image_open surface, synchronously.
  dt_remote_preview_request_t req = { 0 };
  if(!s_calls.render_preview_prepare(&req, &err)) return _handler_fail(err);

  if(!pending)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL,
                                    _("no async transport for this request")));

  if(!s_async.queue_preview(pending, &req, (int)max_px, (int)quality))
    return _handler_fail(_error_new(DT_REMOTE_ERR_PREVIEW_FAILED,
                                    _("could not queue the preview render job")));

  // Deferred: ownership of `pending` moved to the queued job; its
  // main-thread completion responds and releases it.
  return NULL;
}

// Optional boolean param with a default; a present-but-non-boolean value
// is invalid_value (strict), absent takes the default.
static gboolean _optional_bool_default(JsonObject *params, const char *key, gboolean def,
                                       gboolean *out, dt_remote_error_t **err)
{
  if(!params || !json_object_has_member(params, key)) { *out = def; return TRUE; }
  JsonNode *node = json_object_get_member(params, key);
  if(!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
  {
    if(err) *err = _error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter '%s' must be a boolean"), key);
    return FALSE;
  }
  *out = json_node_get_boolean(node);
  return TRUE;
}

static const char *const COMPUTE_SCOPES_KEYS[] =
  { "scopes", "include_summary", "include_bins", "include_images", "image_size", NULL };

// The second asynchronous handler (compute_scopes): validates params,
// checks the darkroom precondition on the main thread (via the calls
// table), and queues the background kernel job (via the async table),
// deferring by returning NULL with no error set. Reuses render_preview's
// pending lifecycle exactly.
static JsonNode *_handler_compute_scopes(JsonObject *params, dt_remote_session_t *session,
                                         dt_remote_pending_t *pending)
{
  (void)session;

  dt_remote_error_t *err = NULL;
  if(!_check_known_keys(params, COMPUTE_SCOPES_KEYS, &err)) return _handler_fail(err);

  // `scopes` is required, a non-empty array of the four known names with
  // no duplicates (protocol reference; duplicates -> invalid_value).
  if(!params || !json_object_has_member(params, "scopes"))
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("missing required parameter 'scopes'")));
  JsonNode *scopes_node = json_object_get_member(params, "scopes");
  if(!scopes_node || !JSON_NODE_HOLDS_ARRAY(scopes_node))
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'scopes' must be an array")));
  JsonArray *scopes_arr = json_node_get_array(scopes_node);
  const guint n = json_array_get_length(scopes_arr);
  if(n == 0)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'scopes' must be non-empty")));

  dt_remote_scopes_request_t req = { 0 };
  for(guint i = 0; i < n; i++)
  {
    JsonNode *item = json_array_get_element(scopes_arr, i);
    if(!item || !JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING)
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("'scopes' entries must be strings")));
    const char *name = json_node_get_string(item);
    gboolean *slot = NULL;
    if(!g_strcmp0(name, "histogram")) slot = &req.want_histogram;
    else if(!g_strcmp0(name, "waveform")) slot = &req.want_waveform;
    else if(!g_strcmp0(name, "parade")) slot = &req.want_parade;
    else if(!g_strcmp0(name, "vectorscope")) slot = &req.want_vectorscope;
    else
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("unknown scope '%s'"), name));
    if(*slot)
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE,
                                      _("duplicate scope '%s'"), name));
    *slot = TRUE;
  }

  if(!_optional_bool_default(params, "include_summary", TRUE, &req.include_summary, &err))
    return _handler_fail(err);
  if(!_optional_bool_default(params, "include_bins", FALSE, &req.include_bins, &err))
    return _handler_fail(err);
  if(!_optional_bool_default(params, "include_images", TRUE, &req.include_images, &err))
    return _handler_fail(err);

  gint64 image_size = 512;
  if(!_optional_int_default(params, "image_size", 512, &image_size, &err)) return _handler_fail(err);
  if(image_size < 128) image_size = 128;
  if(image_size > 1024) image_size = 1024;
  req.image_size = (int)image_size;

  // Main-thread precondition (not_in_darkroom / no_image_open), synchronous.
  if(!s_calls.scopes_prepare(&err)) return _handler_fail(err);

  if(!pending)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INTERNAL,
                                    _("no async transport for this request")));

  if(!s_async.queue_scopes(pending, &req))
    return _handler_fail(_error_new(DT_REMOTE_ERR_SCOPE_FAILED,
                                    _("could not queue the scope computation job")));

  // Deferred: ownership of `pending` moved to the queued job.
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* allowlist (internals §6: 13 entries, static, looked up by g_str_equal)  */
/* ---------------------------------------------------------------------- */

// Every method has a real handler now (compute_scopes was the last one).
// A NULL handler would dispatch to DT_REMOTE_ERR_INTERNAL rather than
// crashing, but the table no longer contains any.
static const dt_remote_method_t g_methods[] = {
  { "hello",                  FALSE, FALSE, FALSE, _handler_hello },
  { "get_state",              FALSE, FALSE, FALSE, _handler_get_state },
  { "list_modules",           TRUE,  FALSE, FALSE, _handler_list_modules },
  { "get_module_schema",      FALSE, FALSE, FALSE, _handler_get_module_schema },
  { "get_module_params",      TRUE,  FALSE, FALSE, _handler_get_module_params },
  { "set_module_params",      TRUE,  TRUE,  FALSE, _handler_set_module_params },
  { "set_module_enabled",     TRUE,  TRUE,  FALSE, _handler_set_module_enabled },
  { "reset_module",           TRUE,  TRUE,  FALSE, _handler_reset_module },
  { "create_module_instance", TRUE,  TRUE,  FALSE, _handler_create_module_instance },
  { "get_history",            TRUE,  FALSE, FALSE, _handler_get_history },
  { "undo",                   TRUE,  TRUE,  FALSE, _handler_undo },
  { "render_preview",         TRUE,  FALSE, TRUE,  _handler_render_preview },
  { "compute_scopes",         TRUE,  FALSE, TRUE,  _handler_compute_scopes },
};

static const dt_remote_method_t *_find_method(const char *name)
{
  for(size_t i = 0; i < G_N_ELEMENTS(g_methods); i++)
    if(g_str_equal(g_methods[i].name, name)) return &g_methods[i];
  return NULL;
}

const dt_remote_method_t *dt_remote_protocol_lookup_method(const char *name)
{
  if(!name) return NULL;
  return _find_method(name);
}

/* ---------------------------------------------------------------------- */
/* wire error envelope                                                     */
/* ---------------------------------------------------------------------- */

static const char *_error_code_to_wire(dt_remote_error_code_t code)
{
  switch(code)
  {
    case DT_REMOTE_ERR_NOT_IN_DARKROOM: return "not_in_darkroom";
    case DT_REMOTE_ERR_NO_IMAGE_OPEN: return "no_image_open";
    case DT_REMOTE_ERR_UNKNOWN_MODULE: return "unknown_module";
    case DT_REMOTE_ERR_UNKNOWN_INSTANCE: return "unknown_instance";
    case DT_REMOTE_ERR_UNKNOWN_FIELD: return "unknown_field";
    case DT_REMOTE_ERR_UNSUPPORTED_FIELD: return "unsupported_field";
    case DT_REMOTE_ERR_INVALID_VALUE: return "invalid_value";
    case DT_REMOTE_ERR_INSTANCE_NOT_SUPPORTED: return "instance_not_supported";
    case DT_REMOTE_ERR_REVISION_CONFLICT: return "revision_conflict";
    case DT_REMOTE_ERR_PREVIEW_FAILED: return "preview_failed";
    case DT_REMOTE_ERR_SCOPE_FAILED: return "scope_failed";
    case DT_REMOTE_ERR_INTERNAL:
    case DT_REMOTE_OK:
    default: return "internal";
  }
}

// Per the protocol reference: retryable is true for revision_conflict,
// busy, and *transient* preview_failed/scope_failed; false otherwise.
// busy is a transport-only code not reachable through dt_remote_error_t
// (remote_edit never sees it). preview_failed's transient-vs-permanent
// distinction needs a hint this error type does not carry, so ALL
// preview_failed errors are reported retryable: render failures from a
// live darkroom are predominantly transient (memory pressure, contention),
// and the design's standing rule is to fail toward retryable (see
// dt_remote_revision_force_bump()'s rationale) -- the worst outcome of a
// wrongly-retryable error is one wasted retry, while wrongly-permanent
// would strand a recoverable client. scope_failed follows the same
// fail-toward-retryable rule (ratified: pin retryable=TRUE
// unconditionally): the empty-capture-slot, cancelled and encode-failure
// modes are all transient conditions another preview run or retry
// resolves.
static gboolean _error_code_retryable(dt_remote_error_code_t code)
{
  return code == DT_REMOTE_ERR_REVISION_CONFLICT || code == DT_REMOTE_ERR_PREVIEW_FAILED
         || code == DT_REMOTE_ERR_SCOPE_FAILED;
}

// The shared envelope body: `wire_code`/`retryable` given directly, for
// the transport-only codes (request_too_large) that dt_remote_error_code_t
// deliberately does not carry.
static JsonNode *_build_error_wire(gboolean has_id, gint64 id, const char *wire_code,
                                   gboolean retryable, const char *message,
                                   const char *details_json)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "id");
  if(has_id) json_builder_add_int_value(b, id);
  else json_builder_add_null_value(b);

  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, FALSE);

  json_builder_set_member_name(b, "error");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "code");
  json_builder_add_string_value(b, wire_code);
  json_builder_set_member_name(b, "message");
  json_builder_add_string_value(b, message ? message : "");

  if(details_json)
  {
    JsonParser *parser = json_parser_new();
    if(json_parser_load_from_data(parser, details_json, -1, NULL))
    {
      json_builder_set_member_name(b, "details");
      json_builder_add_value(b, json_node_copy(json_parser_get_root(parser)));
    }
    g_object_unref(parser);
  }

  json_builder_set_member_name(b, "retryable");
  json_builder_add_boolean_value(b, retryable);
  json_builder_end_object(b);

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

static JsonNode *_build_error(gboolean has_id, gint64 id, dt_remote_error_code_t code,
                              const char *message, const char *details_json)
{
  return _build_error_wire(has_id, id, _error_code_to_wire(code), _error_code_retryable(code),
                           message, details_json);
}

static JsonNode *_build_error_from_dt_error(gboolean has_id, gint64 id, dt_remote_error_t *err)
{
  JsonNode *node = err
    ? _build_error(has_id, id, err->code, err->message, err->details_json)
    : _build_error(has_id, id, DT_REMOTE_ERR_INTERNAL, _("handler failed without an error"), NULL);
  dt_remote_error_free(err);
  return node;
}

static JsonNode *_build_success(gboolean has_id, gint64 id, JsonNode *result)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "id");
  if(has_id) json_builder_add_int_value(b, id);
  else json_builder_add_null_value(b);

  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, TRUE);

  json_builder_set_member_name(b, "result");
  json_builder_add_value(b, result);  // builder takes ownership

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

/* ---------------------------------------------------------------------- */
/* render_preview: response shaping + background job plumbing              */
/* ---------------------------------------------------------------------- */

// Headroom for everything in the response envelope besides the base64
// payload itself (id, ok, mime_type, dims, revision, JSON punctuation) --
// generously above the ~120 bytes those actually take.
#define DT_REMOTE_PREVIEW_ENVELOPE_SLACK ((size_t)512)

gboolean dt_remote_protocol_preview_fits_frame(size_t jpeg_len)
{
  const size_t b64_len = ((jpeg_len + 2) / 3) * 4;  // 4 * ceil(n / 3)
  return b64_len + DT_REMOTE_PREVIEW_ENVELOPE_SLACK <= (size_t)DT_REMOTE_MAX_FRAME;
}

JsonNode *dt_remote_protocol_build_preview_response(gint64 request_id,
                                                    const dt_remote_preview_t *preview,
                                                    const dt_remote_error_t *error)
{
  if(preview)
  {
    // Proactive size check (plan step 9: "the framed response stays below
    // the configured maximum"): a clean, documented request_too_large the
    // client answers by lowering max_px -- rather than encoding megabytes
    // of base64 only for the transport fallback to throw them away. This
    // is genuinely reachable: a 2048x2048 worst-case JPEG buffer is
    // exactly the 16 MiB frame cap before base64's ~4/3 inflation.
    if(!dt_remote_protocol_preview_fits_frame(preview->jpeg_len))
      return _build_error_wire(TRUE, request_id, "request_too_large",
                               FALSE /* per the reference's retryable table */,
                               _("preview is too large to send; retry with a smaller max_px"),
                               NULL);

    gchar *b64 = g_base64_encode(preview->jpeg, preview->jpeg_len);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mime_type");
    json_builder_add_string_value(b, "image/jpeg");
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, preview->width);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, preview->height);
    json_builder_set_member_name(b, "revision");
    json_builder_add_int_value(b, (gint64)preview->revision);
    json_builder_set_member_name(b, "data");
    json_builder_add_string_value(b, b64);
    json_builder_end_object(b);

    JsonNode *result = json_builder_get_root(b);
    g_object_unref(b);
    g_free(b64);

    return _build_success(TRUE, request_id, result);
  }

  if(error) return _build_error(TRUE, request_id, error->code, error->message, error->details_json);

  return _build_error(TRUE, request_id, DT_REMOTE_ERR_INTERNAL,
                      _("preview completion carried no result"), NULL);
}

void dt_remote_protocol_finish_preview(dt_remote_pending_t *pending,
                                       dt_remote_preview_t *preview,
                                       dt_remote_error_t *error)
{
  if(!pending)
  {
    dt_remote_preview_free(preview);
    dt_remote_error_free(error);
    return;
  }

  if(g_cancellable_is_cancelled(pending->cancellable))
  {
    // Disconnect/close won the race: release the result buffers and the
    // pending WITHOUT writing to the closing session (plan step 9:
    // "cancellation on disconnect releases result buffers").
    dt_remote_preview_free(preview);
    dt_remote_error_free(error);
    s_async.complete(pending, NULL);
    return;
  }

  // Revision-coherence check (internals §8, main thread). prepare() stamped
  // the preview with the revision observed the instant it flushed history to
  // the DB; the background export then re-read history from the DB. Revision
  // bumps happen only on the main thread, so if the live revision still equals
  // the stamp at this completion, no newer history could have reached the DB
  // during the render and the pixels are coherent. If it drifted (e.g. a
  // second render_preview's prepare, or a set_module_params, flushed newer
  // history in the queue->run window), the stamp may be stale: discard the
  // rendered payload and fail retryably rather than answer revision R with
  // revision-R+1 pixels (the branch's fail-toward-retryable rule).
  if(preview && s_calls.current_revision
     && s_calls.current_revision() != preview->revision)
  {
    dt_remote_preview_free(preview);
    dt_remote_error_free(error);
    dt_remote_error_t *stale =
      _error_new(DT_REMOTE_ERR_PREVIEW_FAILED,
                 _("darkroom state changed while rendering -- retry"));
    JsonNode *drift = dt_remote_protocol_build_preview_response(pending->request_id, NULL, stale);
    dt_remote_error_free(stale);
    s_async.complete(pending, drift);
    return;
  }

  JsonNode *response;
  if(preview || error)
  {
    response = dt_remote_protocol_build_preview_response(pending->request_id, preview, error);
  }
  else
  {
    // Neither result nor error: the job was discarded before it ever ran
    // (job-system teardown). Session still up -> answer retryably rather
    // than leaving the request dangling until disconnect.
    dt_remote_error_t *discarded =
      _error_new(DT_REMOTE_ERR_PREVIEW_FAILED, _("preview render job was discarded"));
    response = dt_remote_protocol_build_preview_response(pending->request_id, NULL, discarded);
    dt_remote_error_free(discarded);
  }

  dt_remote_preview_free(preview);
  dt_remote_error_free(error);
  s_async.complete(pending, response);
}

// -- the production DT_JOB_QUEUE_SYSTEM_BG job -----------------------------

typedef struct _preview_job_params_t
{
  dt_remote_pending_t *pending;   // owned until handed to the completion
  dt_remote_job_handshake_t *handshake;  // shared with pending->handshake
                                         // (one ref held here); QUEUED ->
                                         // RUNNING claimed by _preview_job_run,
                                         // QUEUED -> RECLAIMED by stop()
  dt_remote_preview_request_t req;
  int max_px, quality;
} _preview_job_params_t;

typedef struct _preview_completion_t
{
  dt_remote_pending_t *pending;
  dt_remote_preview_t *preview;   // owned, nullable
  dt_remote_error_t *error;       // owned, nullable
} _preview_completion_t;

static gboolean _preview_completion_invoke(gpointer data)
{
  _preview_completion_t *c = data;
  dt_remote_protocol_finish_preview(c->pending, c->preview, c->error);
  g_free(c);
  return G_SOURCE_REMOVE;
}

// Marshals one completion to the GLib main context (internals §8). Ownership
// of all three arguments transfers to the main-thread callback; the caller
// must not touch them afterwards -- in particular not `pending`, which the
// callback may free before the calling thread is scheduled again.
//
// Uses g_idle_source_new() + g_source_attach(NULL), NOT g_main_context_invoke:
// invoke runs the callback INLINE on the calling thread whenever that thread
// happens to own the default context, which a worker transiently does during
// dt_remote_server_stop()'s drain (g_main_context_iteration acquires and
// releases ownership each pass). Running _preview_completion_invoke ->
// dt_remote_async_complete on a worker thread would trip its main-thread
// g_assert and abort at shutdown. An attached idle source is only ever
// dispatched by the thread iterating the default context (gtk_main, or the
// stop drain), so the completion is guaranteed to run on the main thread.
static void _preview_queue_completion(dt_remote_pending_t *pending,
                                      dt_remote_preview_t *preview,
                                      dt_remote_error_t *error)
{
  _preview_completion_t *c = g_new0(_preview_completion_t, 1);
  c->pending = pending;
  c->preview = preview;
  c->error = error;

  GSource *source = g_idle_source_new();
  g_source_set_callback(source, _preview_completion_invoke, c, NULL);
  g_source_attach(source, NULL);  // default context; never runs inline here
  g_source_unref(source);
}

static int32_t _preview_job_run(dt_job_t *job)
{
  _preview_job_params_t *p = dt_control_job_get_params(job);

  // Claim the job for running (QUEUED -> RUNNING). If dt_remote_server_stop()
  // already reclaimed it at shutdown (QUEUED -> RECLAIMED), this CAS fails:
  // stop has released the pending on the main thread and it must NOT be
  // touched here -- return, leaving _preview_job_params_free to drop only our
  // own handshake reference and free the params.
  if(!dt_remote_job_handshake_claim_run(p->handshake))
    return 0;

  dt_remote_pending_t *pending = p->pending;
  p->pending = NULL;  // ownership transfers to the completion queued below

  dt_remote_preview_t *preview = NULL;
  dt_remote_error_t *error = NULL;

  // Cancelled before we even started (disconnect while queued): skip the
  // render entirely; finish_preview() sees the fired cancellable and
  // releases everything without writing. Reading the cancellable from
  // this thread is safe: g_cancellable_is_cancelled() is thread-safe, and
  // the pending itself is only freed by the completion this job has not
  // queued yet.
  if(!g_cancellable_is_cancelled(pending->cancellable))
    dt_remote_render_preview_execute(&p->req, p->max_px, p->quality, pending->cancellable,
                                     &preview, &error);

  _preview_queue_completion(pending, preview, error);
  // `pending` must not be touched past this point (see above).
  return 0;
}

static void _preview_job_params_free(void *data)
{
  _preview_job_params_t *p = data;
  // Job discarded without ever running (job-system teardown): the pending is
  // still ours (a job that ran set p->pending = NULL). Claim it first
  // (QUEUED -> RUNNING) so we do not race dt_remote_server_stop()'s reclaim:
  // if stop already won (RECLAIMED), the claim fails and stop owns/released
  // the pending -- we must not touch it. If we win, release it on the main
  // context through the normal completion path (neither result nor error ->
  // a retryable preview_failed) so the session's io_refs/pending_requests
  // accounting stays balanced.
  if(p->pending && dt_remote_job_handshake_claim_run(p->handshake))
    _preview_queue_completion(p->pending, NULL, NULL);
  dt_remote_job_handshake_unref(p->handshake);
  g_free(p);
}

static gboolean _queue_preview_job(dt_remote_pending_t *pending,
                                   const dt_remote_preview_request_t *req,
                                   int max_px, int quality)
{
  dt_job_t *job = dt_control_job_create(_preview_job_run, "remote-edit preview render");
  if(!job) return FALSE;

  _preview_job_params_t *p = g_new0(_preview_job_params_t, 1);
  p->pending = pending;
  // Shared reclaim handshake: one ref stays with the job params, one is
  // published on the pending so dt_remote_server_stop() can reclaim this job
  // if it is still QUEUED when shutdown drains. Set on the main thread (this
  // runs inline in dispatch), before the job can be scheduled.
  p->handshake = dt_remote_job_handshake_new();  // refcount 1 (job side)
  pending->handshake = p->handshake;
  dt_remote_job_handshake_ref(pending->handshake);  // refcount 2 (pending side)
  p->req = *req;
  p->max_px = max_px;
  p->quality = quality;
  dt_control_job_set_params(job, p, _preview_job_params_free);

  // SYSTEM_BG (internals §8): does not contend with user exports on the
  // serialized USER_EXPORT queue, and is never pushed out of the queue.
  // dt_control_add_job() returns TRUE on failure -- but even then it has
  // disposed the job, which ran _preview_job_params_free -> the pending
  // was already released through the discarded-completion path above, so
  // the caller must NOT fail the request a second time.
  dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* compute_scopes: response shaping + background job plumbing              */
/* ---------------------------------------------------------------------- */

// Proactive frame-cap check for the composed scopes response: the base64
// PNGs dominate; bins add ~3*256 numbers (< 40 KiB serialized worst case)
// and the summary/envelope a few hundred bytes -- all covered by generous
// slack. Mirrors dt_remote_protocol_preview_fits_frame's rationale.
#define DT_REMOTE_SCOPES_ENVELOPE_SLACK ((size_t)65536)

static size_t _b64_len(size_t n)
{
  return ((n + 2) / 3) * 4;
}

static gboolean _scopes_fits_frame(const dt_remote_scopes_result_t *r)
{
  size_t total = DT_REMOTE_SCOPES_ENVELOPE_SLACK;
  if(r->waveform.present) total += _b64_len(r->waveform.png_len);
  if(r->parade.present) total += _b64_len(r->parade.png_len);
  if(r->vectorscope.present) total += _b64_len(r->vectorscope.png_len);
  return total <= (size_t)DT_REMOTE_MAX_FRAME;
}

// Emits one key for a requested image scope (waveform/parade/vectorscope).
// With include_images the value carries the rendered PNG
// ({"image": {mime_type,width,height,data}}); without it, a minimal
// metadata object {"image_size": N} states the bounded resolution the
// raster would use -- the smallest-surprise analogue of the histogram's
// minimal {"bins": 256}, so the response always contains one key per
// requested scope even when no image was rendered.
static void _scopes_image_to_json(JsonBuilder *b, const char *key,
                                  const dt_remote_scopes_image_t *img,
                                  int image_size)
{
  json_builder_set_member_name(b, key);
  json_builder_begin_object(b);
  if(img->present)
  {
    json_builder_set_member_name(b, "image");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "mime_type");
    json_builder_add_string_value(b, "image/png");
    json_builder_set_member_name(b, "width");
    json_builder_add_int_value(b, img->width);
    json_builder_set_member_name(b, "height");
    json_builder_add_int_value(b, img->height);
    json_builder_set_member_name(b, "data");
    gchar *b64 = g_base64_encode(img->png, img->png_len);
    json_builder_add_string_value(b, b64);
    g_free(b64);
    json_builder_end_object(b);
  }
  else
  {
    json_builder_set_member_name(b, "image_size");
    json_builder_add_int_value(b, image_size);
  }
  json_builder_end_object(b);
}

JsonNode *dt_remote_protocol_build_scopes_response(gint64 request_id,
                                                   const dt_remote_scopes_result_t *result,
                                                   const dt_remote_error_t *error)
{
  if(result)
  {
    if(!_scopes_fits_frame(result))
      return _build_error_wire(TRUE, request_id, "request_too_large",
                               FALSE /* per the reference's retryable table */,
                               _("scope images are too large to send; retry with a smaller image_size"),
                               NULL);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    // Normative top-level fields (design spec "Scope analysis"): every
    // response states its revision, source stage, color profile and ROI.
    json_builder_set_member_name(b, "revision");
    json_builder_add_int_value(b, (gint64)result->revision);
    json_builder_set_member_name(b, "source");
    json_builder_add_string_value(b, result->source ? result->source : "final_preview");
    json_builder_set_member_name(b, "color_profile");
    json_builder_add_string_value(b, result->color_profile ? result->color_profile : "");
    json_builder_set_member_name(b, "roi");
    json_builder_add_string_value(b, result->roi ? result->roi : "full_image");

    if(result->has_histogram)
    {
      json_builder_set_member_name(b, "histogram");
      json_builder_begin_object(b);
      json_builder_set_member_name(b, "bins");
      json_builder_add_int_value(b, DT_SCOPES_HISTOGRAM_BINS);
      if(result->has_histogram_summary)
      {
        const dt_scopes_histogram_summary_t *s = &result->histogram_summary;
        json_builder_set_member_name(b, "black_clip_fraction");
        json_builder_add_double_value(b, s->black_clip_fraction);
        json_builder_set_member_name(b, "white_clip_fraction");
        json_builder_add_double_value(b, s->white_clip_fraction);
        json_builder_set_member_name(b, "luminance_percentiles");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "p01");
        json_builder_add_double_value(b, s->p01);
        json_builder_set_member_name(b, "p50");
        json_builder_add_double_value(b, s->p50);
        json_builder_set_member_name(b, "p99");
        json_builder_add_double_value(b, s->p99);
        json_builder_end_object(b);
        json_builder_set_member_name(b, "channel_means");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "red");
        json_builder_add_double_value(b, s->mean_red);
        json_builder_set_member_name(b, "green");
        json_builder_add_double_value(b, s->mean_green);
        json_builder_set_member_name(b, "blue");
        json_builder_add_double_value(b, s->mean_blue);
        json_builder_end_object(b);
      }
      if(result->has_histogram_bins)
      {
        // 256 normalized [0,1] bins per RGB channel (design spec: raw
        // counts excluded -- verbose and preview-resolution-dependent).
        static const char *const chan_names[3] = { "red", "green", "blue" };
        json_builder_set_member_name(b, "channel_bins");
        json_builder_begin_object(b);
        for(int ch = 0; ch < 3; ch++)
        {
          json_builder_set_member_name(b, chan_names[ch]);
          json_builder_begin_array(b);
          for(int bin = 0; bin < DT_SCOPES_HISTOGRAM_BINS; bin++)
            json_builder_add_double_value(b, result->histogram_bins[ch][bin]);
          json_builder_end_array(b);
        }
        json_builder_end_object(b);
      }
      json_builder_end_object(b);
    }

    if(result->want_waveform)
      _scopes_image_to_json(b, "waveform", &result->waveform, result->image_size);
    if(result->want_parade)
      _scopes_image_to_json(b, "parade", &result->parade, result->image_size);
    if(result->want_vectorscope)
      _scopes_image_to_json(b, "vectorscope", &result->vectorscope, result->image_size);

    json_builder_end_object(b);
    JsonNode *node = json_builder_get_root(b);
    g_object_unref(b);
    return _build_success(TRUE, request_id, node);
  }

  if(error) return _build_error(TRUE, request_id, error->code, error->message, error->details_json);

  return _build_error(TRUE, request_id, DT_REMOTE_ERR_INTERNAL,
                      _("scopes completion carried no result"), NULL);
}

void dt_remote_protocol_finish_scopes(dt_remote_pending_t *pending,
                                      dt_remote_scopes_result_t *result,
                                      dt_remote_error_t *error)
{
  if(!pending)
  {
    dt_remote_scopes_result_free(result);
    dt_remote_error_free(error);
    return;
  }

  if(g_cancellable_is_cancelled(pending->cancellable))
  {
    // Disconnect/close won the race: release buffers and the pending
    // without writing to the closing session (same as finish_preview).
    dt_remote_scopes_result_free(result);
    dt_remote_error_free(error);
    s_async.complete(pending, NULL);
    return;
  }

  // Deliberately NO completion-time revision drift check here (unlike
  // dt_remote_protocol_finish_preview): every scope in the result derives
  // from the one retained capture buffer and carries the revision stamped
  // at its push, by construction -- the design spec states "If darkroom
  // state changes during computation, the result remains valid for its
  // reported revision."

  JsonNode *response;
  if(result || error)
  {
    response = dt_remote_protocol_build_scopes_response(pending->request_id, result, error);
  }
  else
  {
    // Job discarded before it ever ran (job-system teardown): answer
    // retryably rather than dangling until disconnect.
    dt_remote_error_t *discarded =
      _error_new(DT_REMOTE_ERR_SCOPE_FAILED, _("scope computation job was discarded"));
    response = dt_remote_protocol_build_scopes_response(pending->request_id, NULL, discarded);
    dt_remote_error_free(discarded);
  }

  dt_remote_scopes_result_free(result);
  dt_remote_error_free(error);
  s_async.complete(pending, response);
}

// -- the production DT_JOB_QUEUE_SYSTEM_BG scopes job -----------------------
//
// Same lifecycle as the preview job above (generalized Task 9 machinery):
// the QUEUED->RUNNING / QUEUED->RECLAIMED handshake is shared with the
// pending through pending->handshake, so dt_remote_server_stop()'s reclaim
// walk -- which is keyed only on p->handshake, not on the job type -- sees
// scopes pendings exactly the way it sees preview pendings.

typedef struct _scopes_job_params_t
{
  dt_remote_pending_t *pending;          // owned until handed to the completion
  dt_remote_job_handshake_t *handshake;  // shared with pending->handshake
  dt_remote_scopes_request_t req;
} _scopes_job_params_t;

typedef struct _scopes_completion_t
{
  dt_remote_pending_t *pending;
  dt_remote_scopes_result_t *result;   // owned, nullable
  dt_remote_error_t *error;            // owned, nullable
} _scopes_completion_t;

static gboolean _scopes_completion_invoke(gpointer data)
{
  _scopes_completion_t *c = data;
  dt_remote_protocol_finish_scopes(c->pending, c->result, c->error);
  g_free(c);
  return G_SOURCE_REMOVE;
}

// Marshals one scopes completion to the GLib main context. Same
// g_idle_source_new() + g_source_attach(NULL) discipline as
// _preview_queue_completion (NEVER g_main_context_invoke -- see that
// function's comment for why: invoke would run the main-thread-asserting
// completion inline on a worker during stop()'s drain).
static void _scopes_queue_completion(dt_remote_pending_t *pending,
                                     dt_remote_scopes_result_t *result,
                                     dt_remote_error_t *error)
{
  _scopes_completion_t *c = g_new0(_scopes_completion_t, 1);
  c->pending = pending;
  c->result = result;
  c->error = error;

  GSource *source = g_idle_source_new();
  g_source_set_callback(source, _scopes_completion_invoke, c, NULL);
  g_source_attach(source, NULL);  // default context; never runs inline here
  g_source_unref(source);
}

static int32_t _scopes_job_run(dt_job_t *job)
{
  _scopes_job_params_t *p = dt_control_job_get_params(job);

  // Claim QUEUED -> RUNNING; if stop() already reclaimed (QUEUED ->
  // RECLAIMED) the pending was released on the main thread and must not
  // be touched here (see _preview_job_run).
  if(!dt_remote_job_handshake_claim_run(p->handshake))
    return 0;

  dt_remote_pending_t *pending = p->pending;
  p->pending = NULL;  // ownership transfers to the completion queued below

  dt_remote_scopes_result_t *result = NULL;
  dt_remote_error_t *error = NULL;

  if(!g_cancellable_is_cancelled(pending->cancellable))
    dt_remote_scopes_execute(&p->req, pending->cancellable, &result, &error);

  _scopes_queue_completion(pending, result, error);
  // `pending` must not be touched past this point.
  return 0;
}

static void _scopes_job_params_free(void *data)
{
  _scopes_job_params_t *p = data;
  // Job discarded without ever running: claim it first so we do not race
  // stop()'s reclaim; if we win, release the pending through the normal
  // completion path (neither result nor error -> retryable scope_failed)
  // so io_refs/pending_requests accounting stays balanced.
  if(p->pending && dt_remote_job_handshake_claim_run(p->handshake))
    _scopes_queue_completion(p->pending, NULL, NULL);
  dt_remote_job_handshake_unref(p->handshake);
  g_free(p);
}

static gboolean _queue_scopes_job(dt_remote_pending_t *pending,
                                  const dt_remote_scopes_request_t *req)
{
  dt_job_t *job = dt_control_job_create(_scopes_job_run, "remote-edit scope computation");
  if(!job) return FALSE;

  _scopes_job_params_t *p = g_new0(_scopes_job_params_t, 1);
  p->pending = pending;
  // Shared reclaim handshake, published on the pending exactly like the
  // preview job's, so the type-agnostic reclaim walk in
  // dt_remote_server_stop() covers scopes jobs too.
  p->handshake = dt_remote_job_handshake_new();       // refcount 1 (job side)
  pending->handshake = p->handshake;
  dt_remote_job_handshake_ref(pending->handshake);    // refcount 2 (pending side)
  p->req = *req;
  dt_control_job_set_params(job, p, _scopes_job_params_free);

  // Same queue + failure semantics as _queue_preview_job: even a failed
  // add has disposed the job (releasing the pending via params_free), so
  // the caller must not fail the request a second time.
  dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* dispatch                                                                 */
/* ---------------------------------------------------------------------- */

JsonNode *dt_remote_protocol_dispatch(JsonObject *request, dt_remote_session_t *session)
{
  g_return_val_if_fail(request != NULL, NULL);

  // 1. request id -- validated first so every later error, however the
  // request is otherwise malformed, can still echo it back. An id that
  // cannot be determined (missing, wrong type, negative, non-finite,
  // fractional) is unrecoverable: there is nothing valid to echo, so the
  // response carries `"id": null` -- undocumented by the protocol
  // reference (which only specifies well-formed requests), chosen here as
  // the least-surprising, most-standard fallback for an unidentifiable
  // request.
  gboolean has_id = FALSE;
  gint64 id = 0;
  {
    JsonNode *id_node = json_object_has_member(request, "id") ? json_object_get_member(request, "id") : NULL;
    has_id = id_node && _json_node_get_integer(id_node, &id) && id >= 0;
  }
  if(!has_id)
    return _build_error(FALSE, 0, DT_REMOTE_ERR_INVALID_VALUE, _("missing or invalid request id"), NULL);

  // 2. method name
  const char *method_name = NULL;
  {
    JsonNode *method_node =
      json_object_has_member(request, "method") ? json_object_get_member(request, "method") : NULL;
    if(!method_node || !JSON_NODE_HOLDS_VALUE(method_node)
       || json_node_get_value_type(method_node) != G_TYPE_STRING)
      return _build_error(TRUE, id, DT_REMOTE_ERR_INVALID_VALUE, _("missing or invalid method"), NULL);
    method_name = json_node_get_string(method_node);
    if(strlen(method_name) > DT_REMOTE_PROTOCOL_MAX_STRING_LEN)
      return _build_error(TRUE, id, DT_REMOTE_ERR_INVALID_VALUE, _("method name too long"), NULL);
  }

  // 3. method lookup -- unknown method is a dispatch-level failure, not a
  // per-method domain error, so it is not subject to the error-codes-by-
  // method matrix (there is no method yet to check a row for).
  const dt_remote_method_t *method = _find_method(method_name);
  if(!method)
  {
    gchar *msg = g_strdup_printf(_("unknown method '%s'"), method_name);
    JsonNode *resp = _build_error(TRUE, id, DT_REMOTE_ERR_INVALID_VALUE, msg, NULL);
    g_free(msg);
    return resp;
  }

  if(!method->handler)
  {
    gchar *msg = g_strdup_printf(_("method '%s' is not implemented yet"), method_name);
    JsonNode *resp = _build_error(TRUE, id, DT_REMOTE_ERR_INTERNAL, msg, NULL);
    g_free(msg);
    return resp;
  }

  // 4. params -- NULL if absent, or present but not a JSON object; each
  // handler decides for itself whether that is acceptable (get_state and
  // list_modules take no params and simply ignore it, per the matrix
  // binding that they never emit invalid_value).
  JsonObject *params = NULL;
  if(json_object_has_member(request, "params"))
  {
    JsonNode *params_node = json_object_get_member(request, "params");
    if(params_node && JSON_NODE_HOLDS_OBJECT(params_node)) params = json_node_get_object(params_node);
  }

  // 5. async methods get their pending registered up front -- dispatch is
  // the one who knows the request id; the handler signature does not carry
  // it. A NULL pending (no session, i.e. no async transport) is fine: the
  // handler fails synchronously on it after validating params.
  dt_remote_pending_t *pending = NULL;
  if(method->is_async) pending = s_async.begin(session, id);

  // 6. run the handler
  g_clear_pointer(&s_handler_error, dt_remote_error_free);
  JsonNode *result = method->handler(params, session, pending);

  if(!result)
  {
    if(method->is_async && !s_handler_error)
      return NULL;  // deferred: the pending now belongs to the queued job
    dt_remote_error_t *err = s_handler_error;
    s_handler_error = NULL;
    if(pending) s_async.abort(pending);  // synchronous failure: pending unused
    return _build_error_from_dt_error(TRUE, id, err);
  }

  if(pending) s_async.abort(pending);  // synchronous success: pending unused
  return _build_success(TRUE, id, result);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
