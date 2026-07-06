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
 * cmocka unit tests for the CSPRNG wrapper (src/common/crypto_random.c).
 * Linux-only backend is exercised here (getrandom(2)/dev/urandom
 * fallback); Windows/macOS backends are compiled only on their own
 * platforms and are not reachable from this test run.
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "common/crypto_random.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_fills_32_byte_token_and_is_not_all_zero(void **state)
{
  (void)state;
  uint8_t buf[32];
  memset(buf, 0, sizeof(buf));

  const gboolean ok = dt_crypto_random_bytes(buf, sizeof(buf));
  assert_true(ok);

  // Astronomically unlikely for 32 real random bytes to all be zero;
  // a stub/broken implementation that leaves the buffer untouched (or
  // zeroes it) is exactly the failure mode this guards against.
  gboolean all_zero = TRUE;
  for(size_t i = 0; i < sizeof(buf); i++)
    if(buf[i] != 0) { all_zero = FALSE; break; }
  assert_false(all_zero);
}

static void test_two_calls_produce_different_output(void **state)
{
  (void)state;
  uint8_t a[32], b[32];
  assert_true(dt_crypto_random_bytes(a, sizeof(a)));
  assert_true(dt_crypto_random_bytes(b, sizeof(b)));
  assert_memory_not_equal(a, b, sizeof(a));
}

static void test_zero_length_trivially_succeeds(void **state)
{
  (void)state;
  assert_true(dt_crypto_random_bytes(NULL, 0));
}

static void test_fills_small_and_large_lengths(void **state)
{
  (void)state;
  uint8_t one[1] = { 0 };
  assert_true(dt_crypto_random_bytes(one, sizeof(one)));

  uint8_t big[4096];
  memset(big, 0, sizeof(big));
  assert_true(dt_crypto_random_bytes(big, sizeof(big)));
  gboolean all_zero = TRUE;
  for(size_t i = 0; i < sizeof(big); i++)
    if(big[i] != 0) { all_zero = FALSE; break; }
  assert_false(all_zero);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_fills_32_byte_token_and_is_not_all_zero),
    cmocka_unit_test(test_two_calls_produce_different_output),
    cmocka_unit_test(test_zero_length_trivially_succeeds),
    cmocka_unit_test(test_fills_small_and_large_lengths),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
