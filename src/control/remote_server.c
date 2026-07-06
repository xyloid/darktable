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

#include "control/remote_server.h"

#include "common/crypto_random.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"
#include "control/remote_discovery.h"
#include "control/remote_revision.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* server object (opaque outside this file)                                */
/* ---------------------------------------------------------------------- */

struct dt_remote_server_t
{
  GSocketService *service;    // owned
  guint16 port;
  char *token_b64;             // owned; the hello token is compared against this
  GPtrArray *sessions;          // dt_remote_session_t*, no element free-func --
                                // lifetime is managed explicitly (see
                                // _session_free_now()), never by the array itself
  char *discovery_path;         // owned; full path of the written discovery record
  dt_remote_revision_t revision; // process-local revision tracker (internals §5);
                                // storage lives here, connected/disconnected in
                                // start()/stop(); read through
                                // dt_remote_revision_current(), not this field
                                // directly -- see control/remote_revision.h
};

/* ---------------------------------------------------------------------- */
/* pure helpers (unit-tested directly, no sockets involved)                */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_constant_time_equal(const char *a, size_t alen,
                                       const char *b, size_t blen)
{
  // Length is checked (and may differ) up front -- already visible via
  // the frame's byte length on the wire, so this is not additional
  // leakage. Once lengths match, every byte is compared with no early
  // exit, per internals §7.
  if(alen != blen) return FALSE;
  if(alen == 0) return TRUE;
  if(!a || !b) return FALSE;

  volatile unsigned char acc = 0;
  for(size_t i = 0; i < alen; i++)
    acc |= (unsigned char)a[i] ^ (unsigned char)b[i];
  return acc == 0;
}

char *dt_remote_base64url_encode(const guint8 *data, size_t len)
{
  if(!data && len != 0) return NULL;

  gchar *encoded = g_base64_encode(data, len);
  if(!encoded) return NULL;

  for(gchar *p = encoded; *p; p++)
  {
    if(*p == '+') *p = '-';
    else if(*p == '/') *p = '_';
  }

  // base64url (RFC 4648 §5) omits the '=' padding used by standard base64
  gsize out_len = strlen(encoded);
  while(out_len > 0 && encoded[out_len - 1] == '=') encoded[--out_len] = '\0';

  return encoded;
}

char *dt_remote_server_describe_auth_failure_for_log(int auth_failures,
                                                     const char *attempted_token)
{
  (void)attempted_token;  // intentionally unused: must never appear in diagnostics
  return g_strdup_printf("[remote-edit] hello authentication failed (%d/%d)",
                        auth_failures, DT_REMOTE_SERVER_MAX_AUTH_FAILURES);
}

/* ---------------------------------------------------------------------- */
/* JSON peeking helpers (pre-auth gate; deliberately independent of        */
/* remote_protocol.c's own, private request validation)                    */
/* ---------------------------------------------------------------------- */

static const char *_peek_string_member(JsonObject *obj, const char *name)
{
  if(!obj || !json_object_has_member(obj, name)) return NULL;
  JsonNode *node = json_object_get_member(obj, name);
  if(!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

static JsonObject *_peek_object_member(JsonObject *obj, const char *name)
{
  if(!obj || !json_object_has_member(obj, name)) return NULL;
  JsonNode *node = json_object_get_member(obj, name);
  if(!node || !JSON_NODE_HOLDS_OBJECT(node)) return NULL;
  return json_node_get_object(node);
}

// Mirrors dispatch's own id-validation rule (present, non-negative,
// integral) closely enough to echo a matching id in the transport-level
// error envelopes built in this file -- a separate, small implementation
// rather than reaching into remote_protocol.c's private helpers, since
// these transport-only failures (unauthorized/busy/request_too_large)
// never reach that file at all.
static gboolean _peek_request_id(JsonObject *obj, gboolean *has_id, gint64 *id)
{
  *has_id = FALSE;
  *id = 0;
  if(!obj || !json_object_has_member(obj, "id")) return FALSE;

  JsonNode *node = json_object_get_member(obj, "id");
  if(!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;

  const GType type = json_node_get_value_type(node);
  gint64 value;
  if(type == G_TYPE_INT64)
  {
    value = json_node_get_int(node);
  }
  else if(type == G_TYPE_DOUBLE)
  {
    const double d = json_node_get_double(node);
    if(d != (double)(gint64)d) return FALSE;  // fractional: not a valid id
    value = (gint64)d;
  }
  else
  {
    return FALSE;
  }
  if(value < 0) return FALSE;

  *has_id = TRUE;
  *id = value;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* transport-level wire error envelope                                     */
/* ---------------------------------------------------------------------- */

// Mirrors the shape of remote_protocol.c's (private) error envelope for
// the three transport-only codes that dt_remote_error_t never carries
// (unauthorized, busy, request_too_large) -- see remote_edit.h's comment
// on dt_remote_error_code_t.
static JsonNode *_transport_error(gboolean has_id, gint64 id,
                                  const char *wire_code, const char *message)
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
  json_builder_set_member_name(b, "retryable");
  json_builder_add_boolean_value(b, g_strcmp0(wire_code, "busy") == 0);
  json_builder_end_object(b);

  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_object_unref(b);
  return root;
}

static gboolean _response_is_ok(JsonNode *response)
{
  if(!response || !JSON_NODE_HOLDS_OBJECT(response)) return FALSE;
  JsonObject *obj = json_node_get_object(response);
  return json_object_has_member(obj, "ok") && json_object_get_boolean_member(obj, "ok");
}

static GBytes *_serialize(JsonNode *node)
{
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, node);
  gsize len = 0;
  gchar *data = json_generator_to_data(gen, &len);  // g_malloc'd
  g_object_unref(gen);
  return g_bytes_new_take(data, len);
}

/* ---------------------------------------------------------------------- */
/* session lifecycle                                                       */
/* ---------------------------------------------------------------------- */

static void _session_start_read(dt_remote_session_t *s);
static void _session_start_write(dt_remote_session_t *s, GBytes *framed);
static void _on_read_ready(GObject *source, GAsyncResult *res, gpointer user_data);
static void _on_write_ready(GObject *source, GAsyncResult *res, gpointer user_data);

static void _session_free_now(dt_remote_session_t *s)
{
  g_ptr_array_remove_fast(s->server->sessions, s);  // array has no element free-func
  dt_remote_frame_parser_clear(&s->frame);
  g_clear_pointer(&s->inflight_write, g_bytes_unref);
  while(!g_queue_is_empty(s->write_queue)) g_bytes_unref(g_queue_pop_head(s->write_queue));
  g_queue_free(s->write_queue);
  g_object_unref(s->io_cancellable);
  g_io_stream_close(G_IO_STREAM(s->connection), NULL, NULL);
  g_object_unref(s->connection);
  g_free(s);
}

// Call after every change to `io_refs` (i.e. at the tail of every read/
// write completion callback): finalizes the session exactly once, only
// once no async operation is still in flight against it.
static void _session_after_io_completed(dt_remote_session_t *s)
{
  if(s->closing && s->io_refs == 0) _session_free_now(s);
}

// Idempotent. Marks the session for teardown and cancels any in-flight
// *read* (writes are allowed to finish naturally -- see the header
// comment on dt_remote_session_t::io_cancellable). Never frees `s`
// itself; _session_after_io_completed() does that once safe.
static void _session_request_close(dt_remote_session_t *s)
{
  if(s->closing) return;
  s->closing = TRUE;
  g_cancellable_cancel(s->io_cancellable);
}

/* ---------------------------------------------------------------------- */
/* outbound responses                                                      */
/* ---------------------------------------------------------------------- */

static void _session_queue_write(dt_remote_session_t *s, GBytes *framed /* ownership taken */)
{
  if(s->writing) g_queue_push_tail(s->write_queue, framed);
  else _session_start_write(s, framed);
}

// Takes ownership of `response` (as returned by dt_remote_protocol_dispatch()
// or built locally by _transport_error()). A NULL `response` is the async-
// deferral case (no handler in this step produces it, but the shape is
// honored): nothing to send yet.
static void _session_send(dt_remote_session_t *s, JsonNode *response)
{
  if(!response) return;

  gboolean has_id = FALSE;
  gint64 id = 0;
  if(JSON_NODE_HOLDS_OBJECT(response)) _peek_request_id(json_node_get_object(response), &has_id, &id);

  GBytes *payload = _serialize(response);
  json_node_unref(response);

  GBytes *framed = dt_remote_frame_encode(payload);
  g_bytes_unref(payload);

  if(!framed)
  {
    dt_print(DT_DEBUG_CONTROL,
            "[remote-edit] response exceeds the frame size limit; substituting an error");
    JsonNode *fallback = _transport_error(has_id, id, "request_too_large", _("response too large to send"));
    GBytes *fallback_payload = _serialize(fallback);
    json_node_unref(fallback);
    framed = dt_remote_frame_encode(fallback_payload);
    g_bytes_unref(fallback_payload);
    if(!framed) return;  // the fallback message itself is tiny; should not happen
  }

  _session_queue_write(s, framed);
}

/* ---------------------------------------------------------------------- */
/* per-frame handling: AWAIT_HELLO / READY                                  */
/* ---------------------------------------------------------------------- */

// Returns TRUE to keep the connection (and keep extracting any further
// buffered frames in this read), FALSE to abandon it (per
// dt_remote_frame_cb's contract) -- the only case that returns FALSE is
// exceeding the authentication-failure budget.
static gboolean _handle_pre_auth_frame(dt_remote_session_t *s, JsonObject *req,
                                       gboolean has_id, gint64 id)
{
  const char *method = req ? _peek_string_member(req, "method") : NULL;
  JsonNode *response;
  gboolean authenticated = FALSE;

  if(!req)
  {
    response = _transport_error(has_id, id, "invalid_value", _("malformed request"));
  }
  else if(g_strcmp0(method, "hello") != 0)
  {
    response = _transport_error(has_id, id, "unauthorized", _("hello must be the first frame"));
  }
  else
  {
    JsonObject *params = _peek_object_member(req, "params");
    const char *token = params ? _peek_string_member(params, "token") : NULL;

    if(!token
       || !dt_remote_constant_time_equal(token, strlen(token),
                                         s->server->token_b64, strlen(s->server->token_b64)))
    {
      char *diag = dt_remote_server_describe_auth_failure_for_log(s->auth_failures + 1, token);
      dt_print(DT_DEBUG_CONTROL, "%s", diag);
      g_free(diag);
      response = _transport_error(has_id, id, "unauthorized", _("invalid token"));
    }
    else
    {
      // Token verified -- let the normal hello handler validate the
      // rest (protocol_version, client) and build the wire response;
      // reusing it keeps this file from duplicating that shape.
      response = dt_remote_protocol_dispatch(req, s);
      authenticated = _response_is_ok(response);
      if(authenticated) s->auth_state = DT_REMOTE_SESSION_READY;
    }
  }

  // Any unsuccessful hello burns one attempt from the failure budget,
  // even a validly-tokened one that fails for some other reason (e.g. an
  // unsupported protocol_version) -- the budget's job is to rate-limit
  // an attacker guessing the token, and a legitimate client is expected
  // to get the rest of the handshake right on the first try, so treating
  // every failure mode uniformly keeps this simple without weakening
  // the token-guessing protection.
  gboolean keep_going = TRUE;
  if(!authenticated)
  {
    s->auth_failures++;
    if(s->auth_failures >= DT_REMOTE_SERVER_MAX_AUTH_FAILURES) keep_going = FALSE;
  }

  _session_send(s, response);
  if(!keep_going) _session_request_close(s);

  return keep_going;
}

// Malformed input or backpressure post-auth are never grounds to abandon
// the connection (only auth-failure-budget exhaustion is) -- always
// returns TRUE.
static gboolean _handle_ready_frame(dt_remote_session_t *s, JsonObject *req,
                                    gboolean has_id, gint64 id)
{
  JsonNode *response;

  if(!req)
  {
    response = _transport_error(has_id, id, "invalid_value", _("malformed request"));
  }
  else if(s->pending_requests >= DT_REMOTE_SERVER_MAX_PENDING)
  {
    response = _transport_error(has_id, id, "busy", _("too many in-flight requests"));
  }
  else
  {
    s->pending_requests++;
    response = dt_remote_protocol_dispatch(req, s);
    // NULL means an async handler deferred completion -- not reachable
    // in this step (no async method has a handler yet); when it is, a
    // future job-completion path sends the deferred response and
    // decrements pending_requests itself.
    if(response) s->pending_requests--;
  }

  _session_send(s, response);
  return TRUE;
}

static gboolean _on_frame_cb(GBytes *payload, gpointer user_data)
{
  dt_remote_session_t *s = user_data;

  gsize len = 0;
  gconstpointer data = g_bytes_get_data(payload, &len);

  JsonParser *parser = json_parser_new();
  JsonObject *req = NULL;
  if(json_parser_load_from_data(parser, (const gchar *)data, (gssize)len, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);
    if(root && JSON_NODE_HOLDS_OBJECT(root)) req = json_node_get_object(root);
  }

  gboolean has_id = FALSE;
  gint64 id = 0;
  if(req) _peek_request_id(req, &has_id, &id);

  const gboolean keep_going = (s->auth_state == DT_REMOTE_SESSION_AWAIT_HELLO)
    ? _handle_pre_auth_frame(s, req, has_id, id)
    : _handle_ready_frame(s, req, has_id, id);

  g_object_unref(parser);  // req (borrowed from its tree) must not be used past this point
  return keep_going;
}

/* ---------------------------------------------------------------------- */
/* async read/write loops                                                  */
/* ---------------------------------------------------------------------- */

static void _session_start_read(dt_remote_session_t *s)
{
  s->io_refs++;
  g_input_stream_read_async(s->input, s->read_buf, sizeof(s->read_buf),
                            G_PRIORITY_DEFAULT, s->io_cancellable, _on_read_ready, s);
}

static void _on_read_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  dt_remote_session_t *s = user_data;
  GError *error = NULL;
  const gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), res, &error);
  s->io_refs--;

  if(s->closing)
  {
    g_clear_error(&error);
    _session_after_io_completed(s);
    return;
  }

  if(n <= 0)
  {
    if(n < 0)
      dt_print(DT_DEBUG_CONTROL, "[remote-edit] connection read error: %s",
              error ? error->message : "unknown error");
    else if(s->frame.header_filled != 0 || s->frame.body != NULL)
      // internals §4: a non-empty parser at EOF is a protocol error
      dt_print(DT_DEBUG_CONTROL, "[remote-edit] connection closed mid-frame");
    g_clear_error(&error);
    _session_request_close(s);
    _session_after_io_completed(s);
    return;
  }

  GError *frame_error = NULL;
  if(!dt_remote_frame_feed(&s->frame, s->read_buf, (size_t)n, _on_frame_cb, s, &frame_error))
  {
    dt_print(DT_DEBUG_CONTROL, "[remote-edit] frame error: %s",
            frame_error ? frame_error->message : "unknown error");
    g_clear_error(&frame_error);
    _session_request_close(s);
  }
  // Note: dt_remote_frame_feed() returns TRUE even when _on_frame_cb()
  // returned FALSE to request abandonment ("caller-requested early
  // stop; not an error") -- that decision is only visible via
  // s->closing, which _on_frame_cb() sets through
  // _session_request_close() before returning FALSE.

  if(!s->closing) _session_start_read(s);
  _session_after_io_completed(s);
}

static void _session_start_write(dt_remote_session_t *s, GBytes *framed)
{
  s->io_refs++;
  s->writing = TRUE;
  s->inflight_write = framed;
  gsize len = 0;
  gconstpointer data = g_bytes_get_data(framed, &len);
  // No cancellable: see the header comment on dt_remote_session_t::
  // io_cancellable -- an in-flight write is allowed to finish so a
  // final error response can reach the client even after close begins.
  g_output_stream_write_all_async(s->output, data, len, G_PRIORITY_DEFAULT, NULL,
                                  _on_write_ready, s);
}

static void _on_write_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
  dt_remote_session_t *s = user_data;
  GError *error = NULL;
  g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), res, NULL, &error);
  if(error)
  {
    dt_print(DT_DEBUG_CONTROL, "[remote-edit] connection write error: %s", error->message);
    g_clear_error(&error);
    _session_request_close(s);
  }

  g_clear_pointer(&s->inflight_write, g_bytes_unref);
  s->writing = FALSE;
  s->io_refs--;

  if(!s->closing)
  {
    GBytes *next = g_queue_pop_head(s->write_queue);
    if(next) _session_start_write(s, next);
  }

  _session_after_io_completed(s);
}

/* ---------------------------------------------------------------------- */
/* accept                                                                  */
/* ---------------------------------------------------------------------- */

static gboolean _on_incoming(GSocketService *service, GSocketConnection *connection,
                             GObject *source_object, gpointer user_data)
{
  (void)service;
  (void)source_object;
  dt_remote_server_t *server = user_data;

  if((int)server->sessions->len >= DT_REMOTE_SERVER_MAX_CONNECTIONS)
  {
    // Drop it: we take no reference, so the connection closes when
    // GIO's own (emission-scoped) reference is released after we return.
    return TRUE;
  }

  dt_remote_session_t *session = g_new0(dt_remote_session_t, 1);
  session->connection = g_object_ref(connection);
  session->auth_state = DT_REMOTE_SESSION_AWAIT_HELLO;
  session->server = server;
  session->input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  session->output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  session->io_cancellable = g_cancellable_new();
  session->write_queue = g_queue_new();

  g_ptr_array_add(server->sessions, session);
  _session_start_read(session);

  return TRUE;  // stop other handlers; GSocketService needs nothing further from us
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                                */
/* ---------------------------------------------------------------------- */

dt_remote_server_t *dt_remote_server_start(void)
{
  if(!dt_conf_get_bool("security/enable_remote_control")) return NULL;

  guint8 token_raw[32];
  if(!dt_crypto_random_bytes(token_raw, sizeof(token_raw)))
  {
    dt_print(DT_DEBUG_CONTROL,
            "[remote-edit] failed to generate a session token; remote control disabled");
    return NULL;
  }

  char *token_b64 = dt_remote_base64url_encode(token_raw, sizeof(token_raw));
  memset(token_raw, 0, sizeof(token_raw));
  if(!token_b64)
  {
    dt_print(DT_DEBUG_CONTROL,
            "[remote-edit] failed to encode the session token; remote control disabled");
    return NULL;
  }

  GSocketService *service = g_socket_service_new();
  GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
  GSocketAddress *bind_addr = g_inet_socket_address_new(loopback, 0);
  g_object_unref(loopback);

  GError *error = NULL;
  GSocketAddress *effective = NULL;
  const gboolean bound =
    g_socket_listener_add_address(G_SOCKET_LISTENER(service), bind_addr, G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error);
  g_object_unref(bind_addr);

  if(!bound)
  {
    dt_print(DT_DEBUG_CONTROL, "[remote-edit] failed to bind the loopback listener: %s",
            error ? error->message : "unknown error");
    g_clear_error(&error);
    g_object_unref(service);
    g_free(token_b64);
    return NULL;
  }

  guint16 port = 0;
  if(G_IS_INET_SOCKET_ADDRESS(effective)) port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
  g_clear_object(&effective);

  dt_remote_server_t *server = g_new0(dt_remote_server_t, 1);
  server->service = service;
  server->port = port;
  server->token_b64 = token_b64;
  server->sessions = g_ptr_array_new();
  dt_remote_revision_init(&server->revision);

  char config_dir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(config_dir, sizeof(config_dir));
  server->discovery_path =
    dt_remote_discovery_write(config_dir, port, token_b64, darktable_package_version);

  if(!server->discovery_path)
  {
    dt_print(DT_DEBUG_CONTROL,
            "[remote-edit] failed to write the discovery record; remote control disabled");
    g_object_unref(server->service);
    g_ptr_array_unref(server->sessions);
    g_free(server->token_b64);
    g_free(server);
    return NULL;
  }

  g_signal_connect(service, "incoming", G_CALLBACK(_on_incoming), server);
  dt_remote_revision_connect(&server->revision);

  dt_print(DT_DEBUG_CONTROL, "[remote-edit] listening on 127.0.0.1:%u", (unsigned)port);

  return server;
}

void dt_remote_server_stop(dt_remote_server_t *server)
{
  if(!server) return;

  dt_remote_revision_disconnect(&server->revision);

  g_socket_service_stop(server->service);

  // Request close on every live session, then synchronously pump this
  // main context until each has actually torn itself down (each session
  // removes itself from server->sessions as its last in-flight I/O
  // callback completes -- see _session_free_now()). This keeps shutdown
  // deterministic: by the time this function returns, the discovery
  // record and every connection are gone, and nothing can reference
  // `server` after it is freed below.
  for(guint i = 0; i < server->sessions->len; i++)
    _session_request_close(g_ptr_array_index(server->sessions, i));
  while(server->sessions->len > 0) g_main_context_iteration(NULL, TRUE);

  g_object_unref(server->service);

  dt_remote_discovery_remove(server->discovery_path);
  g_free(server->discovery_path);
  g_free(server->token_b64);
  g_ptr_array_unref(server->sessions);
  g_free(server);
}

guint16 dt_remote_server_get_port(const dt_remote_server_t *server)
{
  return server ? server->port : 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
