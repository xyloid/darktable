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

#include "common/darktable.h"

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
  .get_module_params = dt_remote_get_module_params,
};

static dt_remote_protocol_calls_t s_calls = {
  .get_state = dt_remote_get_state,
  .list_modules = dt_remote_list_modules,
  .get_module_schema = dt_remote_get_module_schema,
  .get_module_params = dt_remote_get_module_params,
};

void dt_remote_protocol_set_calls(const dt_remote_protocol_calls_t *calls)
{
  s_calls = calls ? *calls : DEFAULT_CALLS;
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
  // Empty for now: every named capability ("params", "instances",
  // "history", "preview", "scopes") gates a mutation/async method that
  // does not have a handler yet -- see the allowlist below. Add the
  // matching string here in the same commit that gives its method a
  // real handler.
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
  if(!s_calls.get_module_params(&ref, &values, &err)) return _handler_fail(err);

  // dt_remote_get_module_params() only returns scalar values -- the wire
  // response also needs instance_name/enabled (list_modules) and revision
  // (get_state). Both come from the same live module list this call just
  // walked, so a missing match here means the two calls disagreed --
  // treat that as internal, not as if the module vanished mid-air.
  GPtrArray *modules = NULL;
  dt_remote_error_t *list_err = NULL;
  if(!s_calls.list_modules(&modules, &list_err))
  {
    dt_remote_error_free(list_err);
    g_ptr_array_unref(values);
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
  json_builder_end_object(b);

  JsonNode *result = json_builder_get_root(b);
  g_object_unref(b);
  dt_remote_state_free(state);
  g_ptr_array_unref(modules);
  g_ptr_array_unref(values);
  return result;
}

/* ---------------------------------------------------------------------- */
/* allowlist (internals §6: 13 entries, static, looked up by g_str_equal)  */
/* ---------------------------------------------------------------------- */

// Only hello + the four read methods have a handler so far (plan step 2).
// The remaining eight rows carry their internals-§6 flags now so later
// steps only add a handler, never touch this table's shape. A NULL
// handler dispatches to DT_REMOTE_ERR_INTERNAL rather than crashing.
static const dt_remote_method_t g_methods[] = {
  { "hello",                  FALSE, FALSE, FALSE, _handler_hello },
  { "get_state",              FALSE, FALSE, FALSE, _handler_get_state },
  { "list_modules",           TRUE,  FALSE, FALSE, _handler_list_modules },
  { "get_module_schema",      FALSE, FALSE, FALSE, _handler_get_module_schema },
  { "get_module_params",      TRUE,  FALSE, FALSE, _handler_get_module_params },
  { "set_module_params",      TRUE,  TRUE,  FALSE, NULL },
  { "set_module_enabled",     TRUE,  TRUE,  FALSE, NULL },
  { "reset_module",           TRUE,  TRUE,  FALSE, NULL },
  { "create_module_instance", TRUE,  TRUE,  FALSE, NULL },
  { "get_history",            TRUE,  FALSE, FALSE, NULL },
  { "undo",                   TRUE,  TRUE,  FALSE, NULL },
  { "render_preview",         TRUE,  FALSE, TRUE,  NULL },
  { "compute_scopes",         TRUE,  FALSE, TRUE,  NULL },
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
// (remote_edit never sees it); preview_failed/scope_failed's transient-
// vs-permanent distinction needs a hint this error type does not carry
// yet, and neither code is reachable until render_preview/compute_scopes
// get handlers -- until then, only revision_conflict is retryable.
static gboolean _error_code_retryable(dt_remote_error_code_t code)
{
  return code == DT_REMOTE_ERR_REVISION_CONFLICT;
}

static JsonNode *_build_error(gboolean has_id, gint64 id, dt_remote_error_code_t code,
                              const char *message, const char *details_json)
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
  json_builder_add_string_value(b, _error_code_to_wire(code));
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
  json_builder_add_boolean_value(b, _error_code_retryable(code));
  json_builder_end_object(b);

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
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

  // 5. run the handler
  g_clear_pointer(&s_handler_error, dt_remote_error_free);
  JsonNode *result = method->handler(params, session, NULL);

  if(!result)
  {
    if(method->is_async) return NULL;  // deferred completion; unreachable until an async handler exists
    dt_remote_error_t *err = s_handler_error;
    s_handler_error = NULL;
    return _build_error_from_dt_error(TRUE, id, err);
  }

  return _build_success(TRUE, id, result);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
