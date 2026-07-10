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
 * cmocka unit tests for the remote scopes capture service
 * (src/control/remote_scopes.c), covering the slot-lifecycle logic added
 * for review findings 1 and 2:
 *
 *  - finding 1: the capture gate makes dt_remote_scopes_push() a true no-op
 *    when no remote server is active (nothing is captured).
 *  - finding 2: the pipe-start/push revision equality proof only fills the
 *    slot when the captured pixels provably match their revision stamp,
 *    dropping the push otherwise; the darkroom image-change reset empties
 *    the slot so a previous image's buffer is never served.
 *
 * These exercise the decision logic through a test seam for the revision
 * getter and the active gate -- no develop context, no signal bus (no unit
 * test in this tree initializes darktable.signals; the signal-connect wiring
 * itself is an Xvfb integration concern, per the implementation plan). The
 * push reaches the fill without a develop because the HLG-LUT resolution
 * falls back to an identity LUT when darktable.develop is NULL.
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

#include "common/iop_profile.h"
#include "control/remote_scopes.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// Controllable revision getter (test seam). g_test_rev is what the capture
// reads for both the pipe-start note and the push; tests set it before each
// call to drive the coherent / incoherent branches.
static uint64_t g_test_rev = 0;
static uint64_t _test_revision_getter(void)
{
  return g_test_rev;
}

// A tiny valid final-preview buffer + a linear (matrix) profile so
// _copy_profile takes the cheap nonlinearlut==0 path.
static float _buf[4 * 2 * 2];
static dt_iop_order_iccprofile_info_t _prof;

static void _reset_fixtures(void)
{
  memset(_buf, 0, sizeof(_buf));
  memset(&_prof, 0, sizeof(_prof));
  _prof.nonlinearlut = 0;
  dt_remote_scopes_test_set_revision_getter(_test_revision_getter);
  dt_remote_scopes_capture_reset();
}

static int _setup(void **state)
{
  (void)state;
  _reset_fixtures();
  return 0;
}

static int _teardown(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(FALSE);
  dt_remote_scopes_capture_reset();
  dt_remote_scopes_test_set_revision_getter(NULL);  // restore production getter
  return 0;
}

static void _push_fixture(void)
{
  dt_remote_scopes_push(_buf, 2, 2, &_prof);
}

/* ---------------------------------------------------------------------- */
/* finding 1: the capture gate                                             */
/* ---------------------------------------------------------------------- */

// Gate disabled -> push is a no-op, slot stays empty even with a coherent
// pipe-start note.
static void test_disabled_gate_leaves_slot_empty(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(FALSE);
  g_test_rev = 5;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), -1);
}

/* ---------------------------------------------------------------------- */
/* finding 2: pipe-start / push revision equality proof                    */
/* ---------------------------------------------------------------------- */

// Equal pipe-start and push revisions -> slot fills, stamped with that
// revision.
static void test_coherent_push_fills_slot(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);
  g_test_rev = 7;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();  // push reads 7 too -> coherent
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 7);
}

// A history bump landed during the run (push revision != pipe-start
// revision) -> the push is dropped and the previous coherent slot is kept.
static void test_incoherent_push_is_dropped(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);

  // First, land a coherent buffer at revision 7.
  g_test_rev = 7;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 7);

  // Now a run that starts at 8 but is overtaken by a bump to 9 before push.
  g_test_rev = 8;
  dt_remote_scopes_note_pipe_start();
  g_test_rev = 9;
  _push_fixture();

  // Dropped: the older-but-coherent revision-7 slot is left intact.
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 7);
}

// A push with no paired pipe-start note has no coherence proof -> dropped.
static void test_push_without_note_is_dropped(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);
  g_test_rev = 3;
  _push_fixture();  // no note_pipe_start
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), -1);
}

// The note is one-shot: it is consumed by the first push, so a second push
// (a second gamma hook without a fresh note) is dropped.
static void test_note_is_one_shot(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);
  g_test_rev = 4;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 4);

  // second push without a new note -> dropped, slot unchanged
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 4);
}

/* ---------------------------------------------------------------------- */
/* finding 2 / folded Minor F6: darkroom image-change reset                */
/* ---------------------------------------------------------------------- */

// The darkroom image-change handler resets the slot (tested here through
// dt_remote_scopes_capture_reset(), which the DT_SIGNAL_DEVELOP_IMAGE_CHANGED
// handler calls; the signal wiring itself is an integration concern) so the
// previous image's buffer is never served as the new image's scopes.
static void test_reset_empties_slot(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);
  g_test_rev = 11;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 11);

  dt_remote_scopes_capture_reset();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), -1);
}

// A NULL buffer clears the slot (the producer's explicit clear path).
static void test_null_push_clears_slot(void **state)
{
  (void)state;
  dt_remote_scopes_test_set_active(TRUE);
  g_test_rev = 2;
  dt_remote_scopes_note_pipe_start();
  _push_fixture();
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), 2);

  dt_remote_scopes_push(NULL, 2, 2, &_prof);
  assert_int_equal((int)dt_remote_scopes_test_slot_revision(), -1);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_disabled_gate_leaves_slot_empty, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_coherent_push_fills_slot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_incoherent_push_is_dropped, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_push_without_note_is_dropped, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_note_is_one_shot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_reset_empties_slot, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_null_push_clears_slot, _setup, _teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
