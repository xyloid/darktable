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
  .get_module_params = dt_remote_get_module_params,
  .set_module_params = dt_remote_set_module_params,
  .set_module_enabled = dt_remote_set_module_enabled,
  .reset_module = dt_remote_reset_module,
  .create_module_instance = dt_remote_create_module_instance,
  .get_history = dt_remote_get_history,
  .undo = dt_remote_undo,
  .render_preview_prepare = dt_remote_render_preview_prepare,
  .current_revision = dt_remote_current_revision,
};

static dt_remote_protocol_calls_t s_calls = {
  .get_state = dt_remote_get_state,
  .list_modules = dt_remote_list_modules,
  .get_module_schema = dt_remote_get_module_schema,
  .get_module_params = dt_remote_get_module_params,
  .set_module_params = dt_remote_set_module_params,
  .set_module_enabled = dt_remote_set_module_enabled,
  .reset_module = dt_remote_reset_module,
  .create_module_instance = dt_remote_create_module_instance,
  .get_history = dt_remote_get_history,
  .undo = dt_remote_undo,
  .render_preview_prepare = dt_remote_render_preview_prepare,
  .current_revision = dt_remote_current_revision,
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

static const dt_remote_protocol_async_t DEFAULT_ASYNC = {
  .begin = dt_remote_async_begin,
  .abort = dt_remote_async_abort,
  .complete = dt_remote_async_complete,
  .queue_preview = _queue_preview_job,
};

static dt_remote_protocol_async_t s_async = {
  .begin = dt_remote_async_begin,
  .abort = dt_remote_async_abort,
  .complete = dt_remote_async_complete,
  .queue_preview = _queue_preview_job,
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
  // Each named capability ("params", "instances", "history", "preview",
  // "scopes") is advertised in the same commit that gives its gating
  // method a real handler in the allowlist below. "params" = parameter
  // mutation (set_module_params, plan step 7). "instances" =
  // create_module_instance; "history" = get_history/undo (plan step 8).
  // "preview" = render_preview (plan step 9). Still pending: "scopes"
  // (compute_scopes).
  json_builder_add_string_value(b, "params");
  json_builder_add_string_value(b, "instances");
  json_builder_add_string_value(b, "history");
  json_builder_add_string_value(b, "preview");
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

static const char *const SET_MODULE_PARAMS_KEYS[] =
  { "module", "instance", "values", "expected_revision", "enable", NULL };

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
  GList *value_keys = json_object_get_members(values_obj);
  if(!value_keys)
    return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("'values' must be non-empty")));

  gboolean have_expected_revision = FALSE;
  gint64 expected_revision = 0;
  if(json_object_has_member(params, "expected_revision"))
  {
    if(!_require_int(params, "expected_revision", &expected_revision, &err))
    {
      g_list_free(value_keys);
      return _handler_fail(err);
    }
    if(expected_revision < 0)
    {
      g_list_free(value_keys);
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
      return _handler_fail(_error_new(DT_REMOTE_ERR_INVALID_VALUE, _("parameter 'enable' must be a boolean")));
    }
    enable_value = json_node_get_boolean(enable_node);
    have_enable = TRUE;
  }

  // Resolve field types via the schema -- needed to interpret each raw
  // JSON value (e.g. distinguish an enum's stable-name/int form from a
  // plain int field).
  dt_remote_module_schema_t *schema = NULL;
  if(!s_calls.get_module_schema(module, &schema, &err))
  {
    g_list_free(value_keys);
    return _handler_fail(err);
  }

  dt_remote_patch_t patch = { 0 };
  patch.scalar_values = g_ptr_array_new_with_free_func(dt_remote_patch_entry_free);
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
    return _handler_fail(convert_err);
  }

  const dt_remote_module_ref_t ref = { .op = module, .instance = (int)instance };
  const uint64_t expected_u64 = (uint64_t)expected_revision;

  dt_remote_mutation_result_t *result = NULL;
  gboolean ok = s_calls.set_module_params(&ref, &patch, have_expected_revision ? &expected_u64 : NULL,
                                          &result, &err);
  g_ptr_array_unref(patch.scalar_values);

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

/* ---------------------------------------------------------------------- */
/* allowlist (internals §6: 13 entries, static, looked up by g_str_equal)  */
/* ---------------------------------------------------------------------- */

// Every method except compute_scopes (plan step 9's sibling, still
// pending) has a handler now. A NULL handler dispatches to
// DT_REMOTE_ERR_INTERNAL rather than crashing.
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
// (remote_edit never sees it). preview_failed's transient-vs-permanent
// distinction needs a hint this error type does not carry, so ALL
// preview_failed errors are reported retryable: render failures from a
// live darkroom are predominantly transient (memory pressure, contention),
// and the design's standing rule is to fail toward retryable (see
// dt_remote_revision_force_bump()'s rationale) -- the worst outcome of a
// wrongly-retryable error is one wasted retry, while wrongly-permanent
// would strand a recoverable client. scope_failed stays non-retryable
// until compute_scopes exists to define its failure modes.
static gboolean _error_code_retryable(dt_remote_error_code_t code)
{
  return code == DT_REMOTE_ERR_REVISION_CONFLICT || code == DT_REMOTE_ERR_PREVIEW_FAILED;
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
