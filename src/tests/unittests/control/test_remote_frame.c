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
 * cmocka unit tests for the framing streaming parser and encoder
 * (src/control/remote_frame.c).
 *
 * No JSON, no sockets, no darkroom: this file exercises
 * dt_remote_frame_feed()/dt_remote_frame_encode() against raw byte
 * buffers only, exactly the boundary the design doc calls out as
 * fuzzable standalone. Header bytes for hand-built frames are
 * constructed with plain shifts (`_put_be32`), independent of
 * GUINT32_TO_BE/GUINT32_FROM_BE, so the tests are an independent check
 * of the wire format rather than a tautology against the same macro the
 * implementation uses.
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_frame.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* helpers                                                                 */
/* ---------------------------------------------------------------------- */

// Writes `v` into buf[0..3] as big-endian, independent of host order and
// independent of GUINT32_TO_BE/GUINT32_FROM_BE.
static void _put_be32(uint8_t *buf, uint32_t v)
{
  buf[0] = (uint8_t)(v >> 24);
  buf[1] = (uint8_t)(v >> 16);
  buf[2] = (uint8_t)(v >> 8);
  buf[3] = (uint8_t)(v);
}

// Builds a full hand-rolled wire frame (header + payload) as a GByteArray
// so tests can slice it however they like.
static GByteArray *_build_frame(const uint8_t *payload, uint32_t payload_len)
{
  GByteArray *out = g_byte_array_sized_new(4 + payload_len);
  uint8_t header[4];
  _put_be32(header, payload_len);
  g_byte_array_append(out, header, 4);
  if(payload_len) g_byte_array_append(out, payload, payload_len);
  return out;
}

typedef struct
{
  GPtrArray *payloads;   // collected GBytes* (ref'd), element order preserved
  int stop_after;        // <0 disables early stop
} collector_t;

static void _collector_init(collector_t *c, int stop_after)
{
  c->payloads = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
  c->stop_after = stop_after;
}

static void _collector_clear(collector_t *c)
{
  g_ptr_array_unref(c->payloads);
}

static gboolean _collect_cb(GBytes *payload, gpointer user_data)
{
  collector_t *c = user_data;
  g_ptr_array_add(c->payloads, g_bytes_ref(payload));
  if(c->stop_after >= 0 && (int)c->payloads->len >= c->stop_after) return FALSE;
  return TRUE;
}

static void _assert_collected_bytes_equal(collector_t *c, guint index,
                                          const uint8_t *expected, size_t expected_len)
{
  assert_true(index < c->payloads->len);
  GBytes *got = g_ptr_array_index(c->payloads, index);
  gsize got_len = 0;
  gconstpointer got_data = g_bytes_get_data(got, &got_len);
  assert_int_equal((int)got_len, (int)expected_len);
  if(expected_len) assert_memory_equal(got_data, expected, expected_len);
}

/* ---------------------------------------------------------------------- */
/* encode                                                                  */
/* ---------------------------------------------------------------------- */

static void test_encode_produces_be32_header_and_payload(void **state)
{
  (void)state;
  const uint8_t payload[3] = { 'h', 'i', '!' };
  GBytes *in = g_bytes_new(payload, sizeof(payload));

  GBytes *out = dt_remote_frame_encode(in);
  assert_non_null(out);

  gsize out_len = 0;
  const uint8_t *out_data = g_bytes_get_data(out, &out_len);
  assert_int_equal((int)out_len, 4 + (int)sizeof(payload));

  uint8_t expected_header[4];
  _put_be32(expected_header, sizeof(payload));
  assert_memory_equal(out_data, expected_header, 4);
  assert_memory_equal(out_data + 4, payload, sizeof(payload));

  g_bytes_unref(out);
  g_bytes_unref(in);
}

static void test_encode_length_is_big_endian_regardless_of_host_order(void **state)
{
  (void)state;
  // 300 = 0x0000012C: a length wide enough that a naive host-order write
  // would produce different bytes on little- vs big-endian hosts.
  uint8_t payload[300];
  memset(payload, 0x42, sizeof(payload));
  GBytes *in = g_bytes_new(payload, sizeof(payload));

  GBytes *out = dt_remote_frame_encode(in);
  assert_non_null(out);

  gsize out_len = 0;
  const uint8_t *out_data = g_bytes_get_data(out, &out_len);
  const uint8_t expected_header[4] = { 0x00, 0x00, 0x01, 0x2C };
  assert_memory_equal(out_data, expected_header, 4);

  g_bytes_unref(out);
  g_bytes_unref(in);
}

static void test_encode_rejects_null_payload(void **state)
{
  (void)state;
  assert_null(dt_remote_frame_encode(NULL));
}

static void test_encode_rejects_empty_payload(void **state)
{
  (void)state;
  GBytes *empty = g_bytes_new(NULL, 0);
  assert_null(dt_remote_frame_encode(empty));
  g_bytes_unref(empty);
}

static void test_encode_accepts_max_frame_and_rejects_one_byte_over(void **state)
{
  (void)state;
  uint8_t *buf = g_malloc(DT_REMOTE_MAX_FRAME + 1);
  memset(buf, 0xAB, DT_REMOTE_MAX_FRAME + 1);

  GBytes *at_max = g_bytes_new(buf, DT_REMOTE_MAX_FRAME);
  GBytes *out = dt_remote_frame_encode(at_max);
  assert_non_null(out);
  assert_int_equal((int)g_bytes_get_size(out), (int)(4 + DT_REMOTE_MAX_FRAME));
  g_bytes_unref(out);
  g_bytes_unref(at_max);

  GBytes *over_max = g_bytes_new(buf, DT_REMOTE_MAX_FRAME + 1);
  assert_null(dt_remote_frame_encode(over_max));
  g_bytes_unref(over_max);

  g_free(buf);
}

/* ---------------------------------------------------------------------- */
/* feed: happy path, single call                                           */
/* ---------------------------------------------------------------------- */

static void test_feed_single_frame_one_call(void **state)
{
  (void)state;
  const uint8_t payload[5] = { 1, 2, 3, 4, 5 };
  GByteArray *frame = _build_frame(payload, sizeof(payload));

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  gboolean ok = dt_remote_frame_feed(&p, frame->data, frame->len, _collect_cb, &c, &error);
  assert_true(ok);
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 1);
  _assert_collected_bytes_equal(&c, 0, payload, sizeof(payload));

  // parser is idle again, ready for the next frame
  assert_int_equal((int)p.header_filled, 0);
  assert_null(p.body);
  assert_false(p.failed);

  _collector_clear(&c);
  g_byte_array_unref(frame);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_zero_length_payload_array_is_valid_body(void **state)
{
  // Not to be confused with a zero-length *frame* (rejected below): this
  // just exercises a small, ordinary body to pin down the "smallest
  // legal frame" shape (length 1) plus a slightly larger one in the same
  // call, i.e. two-frames-in-one-read on tiny payloads.
  (void)state;
  const uint8_t p1[1] = { 0x7F };
  const uint8_t p2[2] = { 0x01, 0x02 };
  GByteArray *f1 = _build_frame(p1, sizeof(p1));
  GByteArray *f2 = _build_frame(p2, sizeof(p2));
  GByteArray *both = g_byte_array_new();
  g_byte_array_append(both, f1->data, f1->len);
  g_byte_array_append(both, f2->data, f2->len);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, both->data, both->len, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 2);
  _assert_collected_bytes_equal(&c, 0, p1, sizeof(p1));
  _assert_collected_bytes_equal(&c, 1, p2, sizeof(p2));

  _collector_clear(&c);
  g_byte_array_unref(f1);
  g_byte_array_unref(f2);
  g_byte_array_unref(both);
  dt_remote_frame_parser_clear(&p);
}

/* ---------------------------------------------------------------------- */
/* feed: chunking                                                          */
/* ---------------------------------------------------------------------- */

static void test_feed_header_split_across_reads(void **state)
{
  (void)state;
  const uint8_t payload[4] = { 'w', 'x', 'y', 'z' };
  GByteArray *frame = _build_frame(payload, sizeof(payload));

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  // feed the header 1 byte, 2 bytes, 1 byte -- still no complete frame
  assert_true(dt_remote_frame_feed(&p, frame->data + 0, 1, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);
  assert_int_equal((int)p.header_filled, 1);

  assert_true(dt_remote_frame_feed(&p, frame->data + 1, 2, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);
  assert_int_equal((int)p.header_filled, 3);

  assert_true(dt_remote_frame_feed(&p, frame->data + 3, 1, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);           // header just completed; body not yet fed
  assert_int_equal((int)p.header_filled, 4);
  assert_non_null(p.body);
  assert_int_equal((int)p.body_expected, (int)sizeof(payload));

  // now the payload, all at once
  assert_true(dt_remote_frame_feed(&p, frame->data + 4, sizeof(payload), _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 1);
  _assert_collected_bytes_equal(&c, 0, payload, sizeof(payload));

  _collector_clear(&c);
  g_byte_array_unref(frame);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_body_split_across_reads(void **state)
{
  (void)state;
  const uint8_t payload[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  GByteArray *frame = _build_frame(payload, sizeof(payload));

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  // header in one shot
  assert_true(dt_remote_frame_feed(&p, frame->data, 4, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);

  // body in three uneven chunks
  assert_true(dt_remote_frame_feed(&p, frame->data + 4, 3, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);
  assert_int_equal((int)p.body->len, 3);

  assert_true(dt_remote_frame_feed(&p, frame->data + 7, 4, _collect_cb, &c, &error));
  assert_int_equal((int)c.payloads->len, 0);
  assert_int_equal((int)p.body->len, 7);

  assert_true(dt_remote_frame_feed(&p, frame->data + 11, 3, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 1);
  _assert_collected_bytes_equal(&c, 0, payload, sizeof(payload));
  assert_null(p.body);

  _collector_clear(&c);
  g_byte_array_unref(frame);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_multiple_frames_in_one_read(void **state)
{
  (void)state;
  const uint8_t p1[3] = { 'a', 'b', 'c' };
  const uint8_t p2[6] = { 'd', 'e', 'f', 'g', 'h', 'i' };
  const uint8_t p3[1] = { 'j' };
  GByteArray *f1 = _build_frame(p1, sizeof(p1));
  GByteArray *f2 = _build_frame(p2, sizeof(p2));
  GByteArray *f3 = _build_frame(p3, sizeof(p3));
  GByteArray *all = g_byte_array_new();
  g_byte_array_append(all, f1->data, f1->len);
  g_byte_array_append(all, f2->data, f2->len);
  g_byte_array_append(all, f3->data, f3->len);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, all->data, all->len, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 3);
  _assert_collected_bytes_equal(&c, 0, p1, sizeof(p1));
  _assert_collected_bytes_equal(&c, 1, p2, sizeof(p2));
  _assert_collected_bytes_equal(&c, 2, p3, sizeof(p3));

  _collector_clear(&c);
  g_byte_array_unref(f1);
  g_byte_array_unref(f2);
  g_byte_array_unref(f3);
  g_byte_array_unref(all);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_byte_at_a_time_matches_bulk(void **state)
{
  (void)state;
  const uint8_t p1[7] = { 1, 2, 3, 4, 5, 6, 7 };
  uint8_t p3[130];
  for(size_t i = 0; i < sizeof(p3); i++) p3[i] = (uint8_t)(i * 3 + 1);

  GByteArray *f1 = _build_frame(p1, sizeof(p1));
  GByteArray *f3 = _build_frame(p3, sizeof(p3));
  GByteArray *all = g_byte_array_new();
  g_byte_array_append(all, f1->data, f1->len);
  g_byte_array_append(all, f3->data, f3->len);

  // bulk
  dt_remote_frame_parser_t bulk_p = { 0 };
  collector_t bulk_c;
  _collector_init(&bulk_c, -1);
  GError *bulk_err = NULL;
  assert_true(dt_remote_frame_feed(&bulk_p, all->data, all->len, _collect_cb, &bulk_c, &bulk_err));
  assert_null(bulk_err);

  // byte-at-a-time
  dt_remote_frame_parser_t byte_p = { 0 };
  collector_t byte_c;
  _collector_init(&byte_c, -1);
  GError *byte_err = NULL;
  for(guint i = 0; i < all->len; i++)
  {
    assert_true(dt_remote_frame_feed(&byte_p, all->data + i, 1, _collect_cb, &byte_c, &byte_err));
    assert_null(byte_err);
  }

  assert_int_equal((int)bulk_c.payloads->len, (int)byte_c.payloads->len);
  assert_int_equal((int)bulk_c.payloads->len, 2);
  for(guint i = 0; i < bulk_c.payloads->len; i++)
  {
    GBytes *a = g_ptr_array_index(bulk_c.payloads, i);
    GBytes *b = g_ptr_array_index(byte_c.payloads, i);
    assert_true(g_bytes_equal(a, b));
  }

  // both parsers end up idle
  assert_int_equal((int)bulk_p.header_filled, 0);
  assert_null(bulk_p.body);
  assert_int_equal((int)byte_p.header_filled, 0);
  assert_null(byte_p.body);

  _collector_clear(&bulk_c);
  _collector_clear(&byte_c);
  g_byte_array_unref(f1);
  g_byte_array_unref(f3);
  g_byte_array_unref(all);
  dt_remote_frame_parser_clear(&bulk_p);
  dt_remote_frame_parser_clear(&byte_p);
}

/* ---------------------------------------------------------------------- */
/* feed: early stop via on_frame returning FALSE                           */
/* ---------------------------------------------------------------------- */

static void test_feed_on_frame_false_stops_early_without_error(void **state)
{
  (void)state;
  const uint8_t p1[2] = { 1, 2 };
  const uint8_t p2[2] = { 3, 4 };
  GByteArray *f1 = _build_frame(p1, sizeof(p1));
  GByteArray *f2 = _build_frame(p2, sizeof(p2));
  GByteArray *all = g_byte_array_new();
  g_byte_array_append(all, f1->data, f1->len);
  g_byte_array_append(all, f2->data, f2->len);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, 1);  // stop after the first frame
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, all->data, all->len, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 1);
  _assert_collected_bytes_equal(&c, 0, p1, sizeof(p1));

  // parser is idle (the second frame's bytes were simply not consumed)
  assert_int_equal((int)p.header_filled, 0);
  assert_null(p.body);
  assert_false(p.failed);

  _collector_clear(&c);
  g_byte_array_unref(f1);
  g_byte_array_unref(f2);
  g_byte_array_unref(all);
  dt_remote_frame_parser_clear(&p);
}

/* ---------------------------------------------------------------------- */
/* feed: zero-length / oversized rejection                                 */
/* ---------------------------------------------------------------------- */

static void test_feed_zero_length_frame_rejected(void **state)
{
  (void)state;
  uint8_t header[4];
  _put_be32(header, 0);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  gboolean ok = dt_remote_frame_feed(&p, header, sizeof(header), _collect_cb, &c, &error);
  assert_false(ok);
  assert_non_null(error);
  assert_int_equal(error->domain, DT_REMOTE_FRAME_ERROR);
  assert_int_equal(error->code, DT_REMOTE_FRAME_ERROR_ZERO_LENGTH);
  assert_int_equal((int)c.payloads->len, 0);
  assert_true(p.failed);
  assert_null(p.body);  // never allocated

  g_clear_error(&error);

  // parser is now stuck: further feeds fail immediately, no resync attempt
  gboolean ok2 = dt_remote_frame_feed(&p, header, sizeof(header), _collect_cb, &c, &error);
  assert_false(ok2);
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_FRAME_ERROR_FAILED);
  g_clear_error(&error);

  _collector_clear(&c);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_oversized_frame_rejected_before_allocation(void **state)
{
  (void)state;
  uint8_t header[4];
  _put_be32(header, DT_REMOTE_MAX_FRAME + 1);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  gboolean ok = dt_remote_frame_feed(&p, header, sizeof(header), _collect_cb, &c, &error);
  assert_false(ok);
  assert_non_null(error);
  assert_int_equal(error->domain, DT_REMOTE_FRAME_ERROR);
  assert_int_equal(error->code, DT_REMOTE_FRAME_ERROR_TOO_LARGE);
  assert_true(p.failed);
  assert_null(p.body);  // rejected before allocation, never allocated

  g_clear_error(&error);
  _collector_clear(&c);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_exactly_max_frame_accepted(void **state)
{
  (void)state;
  uint8_t *payload = g_malloc(DT_REMOTE_MAX_FRAME);
  memset(payload, 0x5A, DT_REMOTE_MAX_FRAME);
  GByteArray *frame = _build_frame(payload, DT_REMOTE_MAX_FRAME);

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, frame->data, frame->len, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 1);
  GBytes *got = g_ptr_array_index(c.payloads, 0);
  assert_int_equal((int)g_bytes_get_size(got), (int)DT_REMOTE_MAX_FRAME);

  _collector_clear(&c);
  g_byte_array_unref(frame);
  g_free(payload);
  dt_remote_frame_parser_clear(&p);
}

/* ---------------------------------------------------------------------- */
/* feed: connection-close-mid-frame is caller-visible parser state         */
/* ---------------------------------------------------------------------- */

static void test_feed_partial_header_leaves_parser_visibly_incomplete(void **state)
{
  (void)state;
  const uint8_t partial_header[2] = { 0x00, 0x00 };

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, partial_header, sizeof(partial_header), _collect_cb, &c, &error));
  assert_null(error);

  // simulated EOF here: a caller must treat this as "connection closed
  // mid-frame" because the parser is not idle.
  gboolean idle = (p.header_filled == 0 && p.body == NULL);
  assert_false(idle);
  assert_int_equal((int)p.header_filled, 2);
  assert_null(p.body);
  assert_false(p.failed);

  _collector_clear(&c);
  dt_remote_frame_parser_clear(&p);
}

static void test_feed_partial_body_leaves_parser_visibly_incomplete(void **state)
{
  (void)state;
  const uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  GByteArray *frame = _build_frame(payload, sizeof(payload));

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  // header + first half of the body only
  assert_true(dt_remote_frame_feed(&p, frame->data, 4 + 4, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 0);

  gboolean idle = (p.header_filled == 0 && p.body == NULL);
  assert_false(idle);
  assert_non_null(p.body);
  assert_int_equal((int)p.body->len, 4);
  assert_int_equal((int)p.body_expected, (int)sizeof(payload));
  assert_false(p.failed);

  _collector_clear(&c);
  g_byte_array_unref(frame);
  dt_remote_frame_parser_clear(&p);
}

/* ---------------------------------------------------------------------- */
/* parser_clear                                                            */
/* ---------------------------------------------------------------------- */

static void test_parser_clear_resets_and_frees_partial_body(void **state)
{
  (void)state;
  const uint8_t payload[20] = { 0 };
  GByteArray *frame = _build_frame(payload, sizeof(payload));

  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, frame->data, 4 + 5, _collect_cb, &c, &error));
  assert_non_null(p.body);

  dt_remote_frame_parser_clear(&p);
  assert_null(p.body);
  assert_int_equal((int)p.header_filled, 0);
  assert_int_equal((int)p.body_expected, 0);
  assert_false(p.failed);

  // clearing an already-clear parser is a harmless no-op
  dt_remote_frame_parser_clear(&p);
  assert_null(p.body);

  _collector_clear(&c);
  g_byte_array_unref(frame);
}

static void test_parser_clear_is_null_safe(void **state)
{
  (void)state;
  dt_remote_frame_parser_clear(NULL);
}

/* ---------------------------------------------------------------------- */
/* feed: defensive input handling                                          */
/* ---------------------------------------------------------------------- */

static void test_feed_zero_length_input_is_a_no_op(void **state)
{
  (void)state;
  dt_remote_frame_parser_t p = { 0 };
  collector_t c;
  _collector_init(&c, -1);
  GError *error = NULL;

  assert_true(dt_remote_frame_feed(&p, NULL, 0, _collect_cb, &c, &error));
  assert_null(error);
  assert_int_equal((int)c.payloads->len, 0);
  assert_int_equal((int)p.header_filled, 0);
  assert_null(p.body);

  _collector_clear(&c);
  dt_remote_frame_parser_clear(&p);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_encode_produces_be32_header_and_payload),
    cmocka_unit_test(test_encode_length_is_big_endian_regardless_of_host_order),
    cmocka_unit_test(test_encode_rejects_null_payload),
    cmocka_unit_test(test_encode_rejects_empty_payload),
    cmocka_unit_test(test_encode_accepts_max_frame_and_rejects_one_byte_over),

    cmocka_unit_test(test_feed_single_frame_one_call),
    cmocka_unit_test(test_feed_zero_length_payload_array_is_valid_body),

    cmocka_unit_test(test_feed_header_split_across_reads),
    cmocka_unit_test(test_feed_body_split_across_reads),
    cmocka_unit_test(test_feed_multiple_frames_in_one_read),
    cmocka_unit_test(test_feed_byte_at_a_time_matches_bulk),

    cmocka_unit_test(test_feed_on_frame_false_stops_early_without_error),

    cmocka_unit_test(test_feed_zero_length_frame_rejected),
    cmocka_unit_test(test_feed_oversized_frame_rejected_before_allocation),
    cmocka_unit_test(test_feed_exactly_max_frame_accepted),

    cmocka_unit_test(test_feed_partial_header_leaves_parser_visibly_incomplete),
    cmocka_unit_test(test_feed_partial_body_leaves_parser_visibly_incomplete),

    cmocka_unit_test(test_parser_clear_resets_and_frees_partial_body),
    cmocka_unit_test(test_parser_clear_is_null_safe),

    cmocka_unit_test(test_feed_zero_length_input_is_a_no_op),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
