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
 * of the lifecycle entry points. Socket accept/auth paths are
 * integration-level and are exercised by a real client in a later step
 * (per the implementation plan's acceptance gate).
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
