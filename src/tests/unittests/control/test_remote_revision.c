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
 * cmocka unit tests for the process-local revision tracker
 * (src/control/remote_revision.c), per the remote-edit internals design
 * doc §5 and plan step 6.
 *
 * dt_remote_revision_bump()/dt_remote_revision_stamp_image() are the
 * pure logic a DT_SIGNAL_DEVELOP_HISTORY_CHANGE/DT_SIGNAL_DEVELOP_IMAGE_CHANGED
 * callback invokes; calling them directly here is how these tests
 * simulate "a user-originated history change happened" and "an image
 * switch happened" without a live darktable.signals system (no unit
 * test in this tree initializes darktable.signals -- see
 * test_remote_server.c's own comment on why socket/signal-level
 * behavior is integration-only). The signal-connect wiring itself
 * (dt_remote_revision_connect/disconnect) is exercised by the later
 * Xvfb integration suite, per the implementation plan.
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <cmocka.h>

#include "../util/assert.h"

#include "control/remote_revision.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* init / process-start invariants                                         */
/* ---------------------------------------------------------------------- */

static void test_init_starts_at_revision_zero_with_no_image(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  assert_int_equal((int)dt_remote_revision_get(&rev), 0);
  assert_int_equal(dt_remote_revision_get_imgid(&rev), NO_IMGID);
}

/* ---------------------------------------------------------------------- */
/* NULL-safety (matches the convention in test_remote_server.c)            */
/* ---------------------------------------------------------------------- */

static void test_get_of_null_is_zero(void **state)
{
  (void)state;
  assert_int_equal((int)dt_remote_revision_get(NULL), 0);
}

static void test_get_imgid_of_null_is_no_imgid(void **state)
{
  (void)state;
  assert_int_equal(dt_remote_revision_get_imgid(NULL), NO_IMGID);
}

static void test_bump_of_null_does_not_crash(void **state)
{
  (void)state;
  dt_remote_revision_bump(NULL);
}

static void test_stamp_image_of_null_does_not_crash(void **state)
{
  (void)state;
  dt_remote_revision_stamp_image(NULL, 42);
}

static void test_matches_of_null_is_false(void **state)
{
  (void)state;
  assert_false(dt_remote_revision_matches(NULL, 0, NO_IMGID));
}

/* ---------------------------------------------------------------------- */
/* user-originated change simulation (DEVELOP_HISTORY_CHANGE)               */
/* ---------------------------------------------------------------------- */

static void test_bump_increments_monotonically(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);

  for(int i = 1; i <= 5; i++)
  {
    dt_remote_revision_bump(&rev);
    assert_int_equal((int)dt_remote_revision_get(&rev), i);
  }
}

// A plain history-change bump must never touch the stamped imgid --
// only DEVELOP_IMAGE_CHANGED (dt_remote_revision_stamp_image) does.
static void test_bump_does_not_change_imgid(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_stamp_image(&rev, 7);

  dt_remote_revision_bump(&rev);
  dt_remote_revision_bump(&rev);

  assert_int_equal(dt_remote_revision_get_imgid(&rev), 7);
  assert_int_equal((int)dt_remote_revision_get(&rev), 3);
}

/* ---------------------------------------------------------------------- */
/* image switches (DEVELOP_IMAGE_CHANGED)                                  */
/* ---------------------------------------------------------------------- */

// Plan step 6 requirement: "image switches also change the observable
// revision/state identity" -- stamping the same imgid again still bumps
// the counter (a switch away and back is still an observable change),
// and a genuine switch changes both fields together.
static void test_stamp_image_increments_and_sets_imgid(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);

  dt_remote_revision_stamp_image(&rev, 101);
  assert_int_equal((int)dt_remote_revision_get(&rev), 1);
  assert_int_equal(dt_remote_revision_get_imgid(&rev), 101);

  dt_remote_revision_stamp_image(&rev, 202);
  assert_int_equal((int)dt_remote_revision_get(&rev), 2);
  assert_int_equal(dt_remote_revision_get_imgid(&rev), 202);
}

static void test_stamp_image_same_imgid_still_bumps_counter(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);

  dt_remote_revision_stamp_image(&rev, 101);
  dt_remote_revision_stamp_image(&rev, 101);

  assert_int_equal((int)dt_remote_revision_get(&rev), 2);
  assert_int_equal(dt_remote_revision_get_imgid(&rev), 101);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_revision_matches: stale expected_revision / expected_imgid     */
/* (implemented now, per internals §5, ahead of its plan-step-7 call site) */
/* ---------------------------------------------------------------------- */

static void test_matches_true_when_revision_and_imgid_both_current(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_stamp_image(&rev, 5);

  assert_true(dt_remote_revision_matches(&rev, 1, 5));
}

static void test_matches_false_on_stale_revision_same_image(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_stamp_image(&rev, 5);  // counter=1, imgid=5
  dt_remote_revision_bump(&rev);            // counter=2, imgid=5 (e.g. a history change)

  // client still thinks the revision is 1 (stale) even though it has the
  // right image -- rejected.
  assert_false(dt_remote_revision_matches(&rev, 1, 5));
  assert_true(dt_remote_revision_matches(&rev, 2, 5));
}

static void test_matches_false_on_wrong_imgid_even_with_matching_counter(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_stamp_image(&rev, 5);  // counter=1, imgid=5

  // the client's remembered counter value happens to match, but it was
  // captured against a different image (e.g. it switched away and back,
  // landing on the same counter value some other image now also has) --
  // must still be rejected because the imgid differs.
  assert_false(dt_remote_revision_matches(&rev, 1, 999));
}

/* ---------------------------------------------------------------------- */
/* overflow policy: 64-bit monotonic, silent wraparound (documented, not   */
/* a practical concern -- see the header comment)                         */
/* ---------------------------------------------------------------------- */

static void test_bump_wraps_silently_past_uint64_max(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_test_set_counter(&rev, UINT64_MAX);

  dt_remote_revision_bump(&rev);
  assert_int_equal((int)dt_remote_revision_get(&rev), 0);

  dt_remote_revision_bump(&rev);
  assert_int_equal((int)dt_remote_revision_get(&rev), 1);
}

// A stale expected_revision comparison after a hypothetical wrap is
// rejected the same way any other stale value is -- there is no
// generation counter or ordering logic to special-case wraparound.
static void test_matches_stale_after_wrap_is_still_rejected(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);
  dt_remote_revision_stamp_image(&rev, 3);
  dt_remote_revision_test_set_counter(&rev, UINT64_MAX);
  dt_remote_revision_bump(&rev);  // wraps to 0

  assert_false(dt_remote_revision_matches(&rev, UINT64_MAX, 3));
  assert_true(dt_remote_revision_matches(&rev, 0, 3));
}

/* ---------------------------------------------------------------------- */
/* thread safety: concurrent bumpers must not lose or duplicate increments */
/* ---------------------------------------------------------------------- */

#define THREAD_COUNT 8
#define BUMPS_PER_THREAD 10000

static gpointer _bump_worker(gpointer data)
{
  dt_remote_revision_t *rev = data;
  for(int i = 0; i < BUMPS_PER_THREAD; i++) dt_remote_revision_bump(rev);
  return NULL;
}

static void test_concurrent_bumps_are_not_lost(void **state)
{
  (void)state;
  dt_remote_revision_t rev;
  dt_remote_revision_init(&rev);

  GThread *threads[THREAD_COUNT];
  for(int i = 0; i < THREAD_COUNT; i++)
    threads[i] = g_thread_new(NULL, _bump_worker, &rev);
  for(int i = 0; i < THREAD_COUNT; i++)
    g_thread_join(threads[i]);

  assert_int_equal((long long)dt_remote_revision_get(&rev),
                   (long long)THREAD_COUNT * BUMPS_PER_THREAD);
}

/* ---------------------------------------------------------------------- */
/* dt_remote_revision_current(): no server running                         */
/* ---------------------------------------------------------------------- */

// No test in this file ever calls dt_remote_revision_connect() (that
// requires darktable.signals -- see the file header comment), so the
// registered tracker starts and stays NULL throughout this whole suite.
static void test_current_is_null_when_never_connected(void **state)
{
  (void)state;
  assert_null(dt_remote_revision_current());
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_init_starts_at_revision_zero_with_no_image),

    cmocka_unit_test(test_get_of_null_is_zero),
    cmocka_unit_test(test_get_imgid_of_null_is_no_imgid),
    cmocka_unit_test(test_bump_of_null_does_not_crash),
    cmocka_unit_test(test_stamp_image_of_null_does_not_crash),
    cmocka_unit_test(test_matches_of_null_is_false),

    cmocka_unit_test(test_bump_increments_monotonically),
    cmocka_unit_test(test_bump_does_not_change_imgid),

    cmocka_unit_test(test_stamp_image_increments_and_sets_imgid),
    cmocka_unit_test(test_stamp_image_same_imgid_still_bumps_counter),

    cmocka_unit_test(test_matches_true_when_revision_and_imgid_both_current),
    cmocka_unit_test(test_matches_false_on_stale_revision_same_image),
    cmocka_unit_test(test_matches_false_on_wrong_imgid_even_with_matching_counter),

    cmocka_unit_test(test_bump_wraps_silently_past_uint64_max),
    cmocka_unit_test(test_matches_stale_after_wrap_is_still_rejected),

    cmocka_unit_test(test_concurrent_bumps_are_not_lost),

    cmocka_unit_test(test_current_is_null_when_never_connected),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
