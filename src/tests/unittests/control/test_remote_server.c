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
/*
 * cmocka unit tests for the pure, socket-free helpers in
 * src/control/remote_server.c: constant-time token compare, base64url
 * encoding, and redacted auth-failure diagnostics, plus the NULL-safety
 * of the lifecycle entry points, plus the async pending lifecycle
 * (dt_remote_async_begin/complete/abort and cancellation-on-close),
 * exercised against a socket-free fake session. Socket accept/auth paths
 * are integration-level and are exercised by a real client in a later
 * step (per the implementation plan's acceptance gate).
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>
#include <json-glib/json-glib.h>

#include "../util/assert.h"

#include "control/remote_server.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* dt_remote_constant_time_equal                                           */
/* ---------------------------------------------------------------------- */

static void test_constant_time_equal_identical_strings(void **state)
{
  (void)state;
  const char *a = "the-quick-brown-fox";
  assert_true(dt_remote_constant_time_equal(a, strlen(a), a, strlen(a)));
}

static void test_constant_time_equal_different_length_is_false(void **state)
{
  (void)state;
  assert_false(dt_remote_constant_time_equal("short", 5, "longer-string", 13));
}

static void test_constant_time_equal_same_length_different_content(void **state)
{
  (void)state;
  assert_false(dt_remote_constant_time_equal("aaaaaaaa", 8, "aaaaaaab", 8));
}

// A mismatch at the very first byte must be detected exactly like a
// mismatch at the very last byte -- the loop has no early exit.
static void test_constant_time_equal_detects_mismatch_at_any_position(void **state)
{
  (void)state;
  const char *base = "0123456789abcdef";
  const size_t len = strlen(base);
  for(size_t i = 0; i < len; i++)
  {
    char *copy = g_strdup(base);
    copy[i] = copy[i] ^ 0x1;  // flip one bit at position i
    assert_false(dt_remote_constant_time_equal(base, len, copy, len));
    g_free(copy);
  }
}

static void test_constant_time_equal_both_empty_is_true(void **state)
{
  (void)state;
  assert_true(dt_remote_constant_time_equal(NULL, 0, NULL, 0));
  assert_true(dt_remote_constant_time_equal("", 0, "", 0));
}

/* ---------------------------------------------------------------------- */
/* dt_remote_base64url_encode                                              */
/* ---------------------------------------------------------------------- */

static void test_base64url_encode_known_vector(void **state)
{
  (void)state;
  // "hello world" base64-standard is "aGVsbG8gd29ybGQ=" -- base64url of
  // the same bytes drops the trailing '=' (no '+'/'/' to substitute in
  // this particular vector, so pick one with '?' hmm -- keep it simple:
  // this vector alone establishes the no-padding rule.
  const guint8 data[] = "hello world";
  char *encoded = dt_remote_base64url_encode(data, strlen((const char *)data));
  assert_non_null(encoded);
  assert_string_equal(encoded, "aGVsbG8gd29ybGQ");
  g_free(encoded);
}

// Bytes chosen so the standard base64 alphabet would emit both '+' and
// '/' -- 0xFB 0xEF 0xBE maps to "++++" under RFC 4648 rules for these
// specific bit patterns; more directly: 0xff 0xff 0xbe -> base64
// standard "//++" style output. Rather than hand-deriving exact bytes,
// assert the general property instead: the encoded string never
// contains '+', '/', or '=' for a buffer that empirically produces them
// in standard base64.
static void test_base64url_encode_has_no_standard_base64_specials(void **state)
{
  (void)state;
  guint8 data[64];
  for(int i = 0; i < 64; i++) data[i] = (guint8)i * 4 + 3;  // arbitrary, dense byte values

  gchar *std = g_base64_encode(data, sizeof(data));
  // sanity: this buffer actually exercises '+'/'/' in standard base64,
  // otherwise this test would not be testing anything
  assert_true(strchr(std, '+') != NULL || strchr(std, '/') != NULL);
  g_free(std);

  char *encoded = dt_remote_base64url_encode(data, sizeof(data));
  assert_non_null(encoded);
  assert_null(strchr(encoded, '+'));
  assert_null(strchr(encoded, '/'));
  assert_null(strchr(encoded, '='));
  g_free(encoded);
}

static void test_base64url_encode_zero_length_is_empty_string(void **state)
{
  (void)state;
  char *encoded = dt_remote_base64url_encode(NULL, 0);
  assert_non_null(encoded);
  assert_string_equal(encoded, "");
  g_free(encoded);
}

static void test_base64url_encode_null_data_nonzero_len_fails(void **state)
{
  (void)state;
  assert_null(dt_remote_base64url_encode(NULL, 10));
}

static void test_base64url_encode_32_random_bytes_roundtrips_length(void **state)
{
  (void)state;
  guint8 token[32];
  for(int i = 0; i < 32; i++) token[i] = (guint8)(i * 7 + 1);
  char *encoded = dt_remote_base64url_encode(token, sizeof(token));
  assert_non_null(encoded);
  // 32 bytes -> 43 base64 chars with no padding (ceil(32*4/3) - trailing '=')
  assert_int_equal((int)strlen(encoded), 43);
  g_free(encoded);
}

/* ---------------------------------------------------------------------- */
/* log redaction                                                           */
/* ---------------------------------------------------------------------- */

static void test_describe_auth_failure_never_contains_the_token(void **state)
{
  (void)state;
  const char *attempted = "gU3ss3d-t0ken-that-must-never-appear-in-logs";
  char *desc = dt_remote_server_describe_auth_failure_for_log(2, attempted);
  assert_non_null(desc);
  assert_null(strstr(desc, attempted));
  assert_non_null(strstr(desc, "2"));
  g_free(desc);
}

/* ---------------------------------------------------------------------- */
/* async pending lifecycle (dt_remote_async_begin/complete/abort)           */
/* ---------------------------------------------------------------------- */

// A minimal fake session: no sockets, no server object, no GIO streams.
// Two deliberate presets make it safe to drive the real async functions
// against it on this thread:
//
//  - `writing = TRUE`: _session_send()'s queue path then only pushes the
//    framed response onto write_queue (a real in-flight write would pop
//    it later) instead of calling g_output_stream_write_all_async() on
//    the NULL output stream -- and the queue is directly inspectable.
//  - tests that mark the session `closing` first preset `io_refs = 1`
//    (exactly like a real closing session, which always still has its
//    in-flight read: _on_read_ready() decrements io_refs only *after*
//    close is requested), so releasing the pending's own ref never
//    reaches _session_free_now(), which needs the real server/connection
//    objects a fake session does not have.
static dt_remote_session_t *_fake_session(void)
{
  dt_remote_session_t *s = g_new0(dt_remote_session_t, 1);
  s->auth_state = DT_REMOTE_SESSION_READY;
  s->io_cancellable = g_cancellable_new();
  s->write_queue = g_queue_new();
  s->pendings = g_ptr_array_new();
  s->writing = TRUE;
  return s;
}

static void _fake_session_free(dt_remote_session_t *s)
{
  while(!g_queue_is_empty(s->write_queue)) g_bytes_unref(g_queue_pop_head(s->write_queue));
  g_queue_free(s->write_queue);
  g_ptr_array_free(s->pendings, TRUE);
  g_object_unref(s->io_cancellable);
  g_free(s);
}

// a minimal-but-valid response node, as an async completion would build
static JsonNode *_small_response(gint64 id)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, id);
  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_end_object(b);
  JsonNode *n = json_builder_get_root(b);
  g_object_unref(b);
  return n;
}

// asserts exactly one frame is queued, decodes it (4-byte big-endian
// length + JSON payload), and returns the payload object. The returned
// object is owned by *parser_out; unref it when done.
static JsonObject *_parse_single_queued_frame(dt_remote_session_t *s, JsonParser **parser_out)
{
  assert_int_equal((int)g_queue_get_length(s->write_queue), 1);
  GBytes *framed = g_queue_peek_head(s->write_queue);
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(framed, &len);
  assert_true(len > 4);
  const guint32 body_len =
    ((guint32)data[0] << 24) | ((guint32)data[1] << 16) | ((guint32)data[2] << 8) | (guint32)data[3];
  assert_int_equal((int)body_len, (int)(len - 4));

  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, (const gchar *)data + 4, (gssize)body_len, NULL));
  JsonNode *root = json_parser_get_root(parser);
  assert_true(JSON_NODE_HOLDS_OBJECT(root));
  *parser_out = parser;
  return json_node_get_object(root);
}

static void test_async_begin_registers_pending_and_holds_session(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  assert_non_null(pending);
  assert_ptr_equal(pending->session, s);
  assert_int_equal((int)pending->request_id, 42);
  assert_non_null(pending->cancellable);
  assert_false(g_cancellable_is_cancelled(pending->cancellable));

  // the pending is an in-flight op against the session: it must hold the
  // session live exactly like a read/write does (io_refs accounting)
  assert_int_equal(s->io_refs, 1);
  assert_int_equal((int)s->pendings->len, 1);
  assert_ptr_equal(g_ptr_array_index(s->pendings, 0), pending);

  dt_remote_async_complete(pending, NULL);  // release for teardown
  _fake_session_free(s);
}

static void test_async_begin_null_session_returns_null(void **state)
{
  (void)state;
  // no session (e.g. a dispatcher unit test) -> no async transport; the
  // dispatcher maps this to a synchronous error, never a crash
  assert_null(dt_remote_async_begin(NULL, 7));
}

static void test_async_complete_sends_response_and_releases(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;  // as _handle_ready_frame left it before deferral

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  dt_remote_async_complete(pending, _small_response(42));

  // exactly one framed response reached the write path, carrying the id
  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  assert_int_equal((int)json_object_get_int_member(resp, "id"), 42);
  assert_true(json_object_get_boolean_member(resp, "ok"));
  g_object_unref(parser);

  // ... and the bookkeeping was torn down exactly once
  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
}

static void test_async_complete_null_response_releases_only(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  dt_remote_async_complete(pending, NULL);  // cancelled/no-result path

  assert_int_equal((int)g_queue_get_length(s->write_queue), 0);
  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
}

static void test_async_abort_releases_without_touching_pending_requests(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 5;

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  dt_remote_async_abort(pending);

  // abort is the synchronous-outcome path: _handle_ready_frame still owns
  // the pending_requests decrement there, so abort must not touch it
  assert_int_equal(s->pending_requests, 5);
  assert_int_equal(s->io_refs, 0);
  assert_int_equal((int)s->pendings->len, 0);
  assert_int_equal((int)g_queue_get_length(s->write_queue), 0);

  _fake_session_free(s);
}

static void test_session_request_close_fires_pending_cancellables(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->io_refs = 1;  // the in-flight read every real closing session has

  dt_remote_pending_t *p1 = dt_remote_async_begin(s, 1);
  dt_remote_pending_t *p2 = dt_remote_async_begin(s, 2);

  dt_remote_session_request_close(s);

  assert_true(s->closing);
  assert_true(g_cancellable_is_cancelled(p1->cancellable));
  assert_true(g_cancellable_is_cancelled(p2->cancellable));

  dt_remote_async_complete(p1, NULL);
  dt_remote_async_complete(p2, NULL);
  _fake_session_free(s);
}

static void test_async_complete_on_closing_session_drops_response(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->io_refs = 1;  // see _fake_session()'s comment
  s->pending_requests = 1;

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  dt_remote_session_request_close(s);

  // completion after disconnect: the response is released, NOT written to
  // the closing session, and the pending is fully released either way
  dt_remote_async_complete(pending, _small_response(42));

  assert_int_equal((int)g_queue_get_length(s->write_queue), 0);
  assert_int_equal(s->pending_requests, 0);
  assert_int_equal(s->io_refs, 1);  // only the preset read ref remains
  assert_int_equal((int)s->pendings->len, 0);

  _fake_session_free(s);
}

static void test_async_complete_oversized_response_falls_back_to_request_too_large(void **state)
{
  (void)state;
  dt_remote_session_t *s = _fake_session();
  s->pending_requests = 1;

  // a response whose serialized form exceeds DT_REMOTE_MAX_FRAME: the
  // send path must substitute the request_too_large error, not go silent
  const size_t huge_len = (size_t)DT_REMOTE_MAX_FRAME + 1024;
  char *huge = g_malloc(huge_len + 1);
  memset(huge, 'x', huge_len);
  huge[huge_len] = '\0';

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "id");
  json_builder_add_int_value(b, 42);
  json_builder_set_member_name(b, "ok");
  json_builder_add_boolean_value(b, TRUE);
  json_builder_set_member_name(b, "result");
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "data");
  json_builder_add_string_value(b, huge);
  json_builder_end_object(b);
  json_builder_end_object(b);
  JsonNode *response = json_builder_get_root(b);
  g_object_unref(b);
  g_free(huge);

  dt_remote_pending_t *pending = dt_remote_async_begin(s, 42);
  dt_remote_async_complete(pending, response);

  JsonParser *parser = NULL;
  JsonObject *resp = _parse_single_queued_frame(s, &parser);
  assert_int_equal((int)json_object_get_int_member(resp, "id"), 42);
  assert_false(json_object_get_boolean_member(resp, "ok"));
  JsonObject *error = json_object_get_object_member(resp, "error");
  assert_string_equal(json_object_get_string_member(error, "code"), "request_too_large");
  g_object_unref(parser);

  assert_int_equal(s->pending_requests, 0);
  _fake_session_free(s);
}

/* ---------------------------------------------------------------------- */
/* lifecycle NULL-safety (no sockets exercised)                            */
/* ---------------------------------------------------------------------- */

static void test_stop_is_null_safe(void **state)
{
  (void)state;
  dt_remote_server_stop(NULL);  // must not crash
}

static void test_get_port_of_null_server_is_zero(void **state)
{
  (void)state;
  assert_int_equal((int)dt_remote_server_get_port(NULL), 0);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_constant_time_equal_identical_strings),
    cmocka_unit_test(test_constant_time_equal_different_length_is_false),
    cmocka_unit_test(test_constant_time_equal_same_length_different_content),
    cmocka_unit_test(test_constant_time_equal_detects_mismatch_at_any_position),
    cmocka_unit_test(test_constant_time_equal_both_empty_is_true),

    cmocka_unit_test(test_base64url_encode_known_vector),
    cmocka_unit_test(test_base64url_encode_has_no_standard_base64_specials),
    cmocka_unit_test(test_base64url_encode_zero_length_is_empty_string),
    cmocka_unit_test(test_base64url_encode_null_data_nonzero_len_fails),
    cmocka_unit_test(test_base64url_encode_32_random_bytes_roundtrips_length),

    cmocka_unit_test(test_describe_auth_failure_never_contains_the_token),

    cmocka_unit_test(test_async_begin_registers_pending_and_holds_session),
    cmocka_unit_test(test_async_begin_null_session_returns_null),
    cmocka_unit_test(test_async_complete_sends_response_and_releases),
    cmocka_unit_test(test_async_complete_null_response_releases_only),
    cmocka_unit_test(test_async_abort_releases_without_touching_pending_requests),
    cmocka_unit_test(test_session_request_close_fires_pending_cancellables),
    cmocka_unit_test(test_async_complete_on_closing_session_drops_response),
    cmocka_unit_test(test_async_complete_oversized_response_falls_back_to_request_too_large),

    cmocka_unit_test(test_stop_is_null_safe),
    cmocka_unit_test(test_get_port_of_null_server_is_zero),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
