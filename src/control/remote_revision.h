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

// Process-local revision tracker for the darktable MCP remote-edit
// subsystem. See the remote-edit internals design doc §5 (normative
// struct shape) and the implementation plan's step 6.
//
// Purpose: every mutation-capable response (get_state, module/history
// reads, and -- from plan step 7 onward -- mutation results) carries a
// `revision` number so a client can detect "something changed under me"
// and, for mutations, request compare-and-swap semantics via
// `expected_revision`. The counter is NOT a database history number
// (dt_dev_history_end survives across restarts via the database; this
// counter is process-local, is never persisted to disk, and starts at 0
// the first time this process touches it) -- but unlike an earlier
// revision of this design, it does NOT reset every time a remote server
// instance starts: see "Ownership" below.
//
// Ownership: the storage for the single, process-wide tracker instance is
// a static singleton inside remote_revision.c -- it exists for the
// entire lifetime of the darktable process, not just while a remote
// server happens to be running. dt_remote_server_start()/_stop() only
// connect/disconnect the two signal handlers that feed it (see
// control/remote_server.c); they never create, destroy, or reset its
// storage. This matters if a remote server is ever stopped and
// restarted within one process (e.g. a future live toggle of
// `security/enable_remote_control`): a client holding a pre-toggle
// `expected_revision` must not be able to false-match against a counter
// that quietly went back to 0. This header intentionally has no
// dependency on remote_server.h/remote_protocol.h (gio, json-glib):
// remote_edit.c reaches the live tracker only through
// dt_remote_revision_current(), so the read layer never needs to know
// about the server's socket/session machinery.
//
// Thread-safety: darktable's build restricts GLib API to the 2.56
// surface (see GLIB_VERSION_MAX_ALLOWED in src/CMakeLists.txt), and
// GLib's 64-bit atomics (g_atomic_int64_*) were only added in 2.74.
// Internals §5 documents the tracker's `counter` field as "g_atomic
// access"; here that intent is realized with a small static mutex
// (G_LOCK/G_UNLOCK in remote_revision.c) guarding every read and write
// of `counter` and `imgid` together, which gives the same guarantee
// (monotonic, thread-safe increments) plus something g_atomic on two
// independent fields would not: a coherent (revision, imgid) pair for
// dt_remote_revision_matches(). In practice every writer (the two
// signal callbacks) and the ordinary reader (the request handler) run
// on the GLib main context; the lock is belt-and-braces for the
// job-completion paths, exactly as internals §5 describes.
//
// Overflow policy: `counter` is a 64-bit unsigned monotonic counter.
// Wraparound is not a practical concern (at, say, 10 000 increments per
// second -- vastly faster than any plausible edit rate -- wrapping
// 2^64 values takes over 58 million years), but the policy is defined
// anyway: on overflow the counter silently wraps to 0 and continues
// counting with no special-casing, exactly like any other unsigned
// integer in C. A stale `expected_revision` observed after a
// hypothetical wrap is rejected by the same equality check as any other
// stale value -- there is no ordering/generation logic to get confused
// by wraparound.

#pragma once

#include "common/darktable.h"  // dt_imgid_t, NO_IMGID

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

// Normative shape (internals §5): a 64-bit counter and the imgid it was
// last stamped with. Do not add fields here -- synchronization is
// provided externally (remote_revision.c's static lock), not via a
// member of this struct, so the type stays exactly what the design doc
// specifies and stays trivially copyable for tests/fixtures.
typedef struct dt_remote_revision_t
{
  uint64_t counter;   // synchronized access only, via the functions below;
                      // 0 is a valid value at process start.
  dt_imgid_t imgid;   // identity: revision N is meaningful only for the
                      // image it was stamped with; NO_IMGID until the
                      // first DEVELOP_IMAGE_CHANGED.
} dt_remote_revision_t;

/** Resets `rev` to its process-start state: counter 0, imgid NO_IMGID.
 * Not synchronized -- call only before `rev` is reachable from more than
 * one thread. This is a pure fixture helper: tests use it to initialize
 * their own local `dt_remote_revision_t` instances. Production code does
 * NOT call this on the process-wide singleton -- that instance is a
 * static with process lifetime, whose all-zero initial state already
 * equals what this function would set (counter 0, NO_IMGID == 0), and it
 * is intentionally never re-run, so a stopped-and-restarted remote
 * server does not reset the counter. See the file header's "Ownership"
 * paragraph. */
void dt_remote_revision_init(dt_remote_revision_t *rev);

/* ---------------------------------------------------------------------- */
/* pure logic: no signals, no darktable globals -- unit-testable directly  */
/* ---------------------------------------------------------------------- */

/** Increments `rev`'s counter by one. This is what a
 * DT_SIGNAL_DEVELOP_HISTORY_CHANGE callback does; calling it directly is
 * how unit tests simulate "a user-originated history change happened"
 * without a live signal system. NULL-safe (no-op). */
void dt_remote_revision_bump(dt_remote_revision_t *rev);

/** Increments `rev`'s counter by one AND stamps `imgid` as the current
 * image, so an image switch is always observable as a state change even
 * if the new image happens to produce the exact same history contents.
 * This is what a DT_SIGNAL_DEVELOP_IMAGE_CHANGED callback does; calling
 * it directly is how unit tests simulate an image switch. NULL-safe
 * (no-op). */
void dt_remote_revision_stamp_image(dt_remote_revision_t *rev, dt_imgid_t imgid);

/** Returns `rev`'s current counter value, or 0 if `rev` is NULL (e.g. no
 * remote server is running). */
uint64_t dt_remote_revision_get(const dt_remote_revision_t *rev);

/** Returns `rev`'s currently stamped imgid, or NO_IMGID if `rev` is
 * NULL. */
dt_imgid_t dt_remote_revision_get_imgid(const dt_remote_revision_t *rev);

/** Compare-and-swap precondition helper for plan step 7's mutation
 * engine: TRUE iff `rev`'s counter equals `expected_revision` AND its
 * stamped imgid equals `expected_imgid` -- both captured under one lock,
 * so the comparison never sees a torn (revision, imgid) pair. A stale
 * expected_revision (wrong counter) and an image switch since the
 * client last read state (wrong imgid, possibly with the same counter
 * value it remembered) are both rejected here. FALSE if `rev` is NULL.
 * Implemented (and unit-tested) now per internals §5, though no
 * mutation call site invokes it until plan step 7 -- get_state and the
 * read-only handlers never need it. */
gboolean dt_remote_revision_matches(const dt_remote_revision_t *rev,
                                    uint64_t expected_revision,
                                    dt_imgid_t expected_imgid);

/** Test-only: force `rev`'s counter to an arbitrary value (e.g.
 * UINT64_MAX) so overflow behavior can be exercised without looping
 * 2^64 times. Not used by production code -- kept in the public header
 * (rather than a test-private one) only because remote_revision.c has
 * no other seam to reach into the lock-protected counter; the name
 * makes the intent unambiguous at every call site. */
void dt_remote_revision_test_set_counter(dt_remote_revision_t *rev, uint64_t value);

/** Test-only: direct access to the process-wide singleton storage that
 * dt_remote_revision_connect()/_disconnect()/_current() manage, bypassing
 * the "connected" gate -- dt_remote_revision_current() only, so tests can
 * bump/inspect it (e.g. via dt_remote_revision_bump()/_get() above) even
 * when no server is "connected". This is the seam unit tests use to
 * simulate a dt_remote_server_start()/_stop()/_start() cycle and assert
 * the counter is not reset by it, without requiring a live
 * darktable.signals (this tree's unit tests never initialize one -- see
 * the note in remote_revision.c). Never NULL. Not used by production
 * code. */
dt_remote_revision_t *dt_remote_revision_test_singleton(void);

/** Test-only: resets the process-wide singleton back to its process-start
 * state (counter 0, imgid NO_IMGID, "disconnected") so test cases don't
 * leak state into one another through the shared static. Not used by
 * production code -- the whole point of the singleton is that nothing
 * production-side ever does this. */
void dt_remote_revision_test_reset_singleton(void);

/* ---------------------------------------------------------------------- */
/* signal wiring (main-thread only; not unit-tested -- see remote_server.c */
/* and the note in remote_revision.c; covered by later integration tests)  */
/* ---------------------------------------------------------------------- */

/** Connects the process-wide singleton (see the file header's "Ownership"
 * paragraph) to DT_SIGNAL_DEVELOP_HISTORY_CHANGE (bump) and
 * DT_SIGNAL_DEVELOP_IMAGE_CHANGED (bump + re-stamp imgid from
 * darktable.develop), and makes it the tracker dt_remote_revision_current()
 * returns. Call once per dt_remote_server_start(), on the main context,
 * after the signal system and darktable.develop both exist, mirroring the
 * connect pattern at src/libs/history.c:165. Idempotent: a second call
 * before a matching dt_remote_revision_disconnect() is a no-op (it does
 * NOT double-subscribe the signal handlers) -- this guards against a
 * future start()-without-stop() bug turning every history change into a
 * double bump. Does not touch the singleton's counter/imgid: connecting
 * (or reconnecting) never resets them. */
void dt_remote_revision_connect(void);

/** Disconnects everything dt_remote_revision_connect() wired up, and
 * clears dt_remote_revision_current() (back to NULL) until the next
 * connect(). NULL-safe to call when never connected (a no-op). Call from
 * dt_remote_server_stop(). Does not touch the singleton's counter/imgid
 * -- the storage survives disconnect() by construction, so a subsequent
 * dt_remote_revision_connect() picks up exactly where the counter left
 * off, per the file header's "Ownership" paragraph. */
void dt_remote_revision_disconnect(void);

/** Plan step 7's mutation-engine counterpart to a signal-driven bump.
 *
 * `dt_control_signal_raise()` delivers DT_SIGNAL_DEVELOP_HISTORY_CHANGE
 * asynchronously: its `synchronous` flag is FALSE for that signal (see
 * `_signal_description` in src/control/signal.c), so even when
 * `dt_dev_add_history_item()` is called from the GTK main thread, the
 * resulting `_on_history_change()` bump is queued via
 * `g_main_context_invoke_full(NULL, G_PRIORITY_HIGH_IDLE, ...)` and does
 * NOT run before the calling function returns -- it runs on a later
 * iteration of the main loop, which (since the client only ever sees the
 * response after the socket write goes out through that same main loop)
 * reliably happens *before* the client's next request arrives. A mutation
 * handler that simply returned `dt_remote_revision_get(...)` after calling
 * `dt_dev_add_history_item()` would therefore report a revision that is
 * stale by the time it is read (still one behind), or -- worse -- if it
 * waited and read again, would race the client's next `expected_revision`
 * against a counter that a redundant queued bump is about to advance out
 * from under it (spurious `revision_conflict` on a request that changed
 * nothing else).
 *
 * The fix: the mutation engine calls this function immediately after
 * `dt_dev_add_history_item()` lands its one history item, to bump the
 * singleton's counter *synchronously* and use the returned value as the
 * resulting revision. To keep the later, now-redundant
 * DT_SIGNAL_DEVELOP_HISTORY_CHANGE delivery from double-counting the same
 * mutation, this call also arms one slot of "expect one more signal
 * delivery for a change already accounted for" -- `_on_history_change()`
 * consumes one such slot instead of bumping again when one is armed. Any
 * *other* history change (e.g. the user editing via the GUI while a
 * client is connected) still arrives with no slot armed and bumps
 * normally, exactly once. Monotonicity is preserved either way: this is
 * strictly additive bookkeeping around the same counter, never a reset or
 * a rewind.
 *
 * Always operates on the process-wide singleton directly (like the real
 * signal handler and the test-only simulation seam below), independent of
 * whether `dt_remote_revision_current()` currently returns non-NULL -- see
 * the "Ownership" paragraph: the singleton's storage is bumped regardless
 * of connection state. In production this is only reachable through the
 * live mutation engine, which itself is only reachable while a server is
 * connected, so the distinction is moot there. Returns the new counter
 * value directly, so a call site never needs a separate
 * `dt_remote_revision_get()` afterwards. Call this exactly once per
 * mutation, after the single `dt_dev_add_history_item()` call that commits
 * it, on the main thread. */
uint64_t dt_remote_revision_commit_history_change(void);

/** Test-only: runs the exact logic dt_remote_revision_connect()'s
 * DEVELOP_HISTORY_CHANGE handler runs -- including the one-shot
 * suppression check dt_remote_revision_commit_history_change() arms --
 * without requiring a live signal connection (this tree's unit tests never
 * initialize darktable.signals; see the note in remote_revision.c and in
 * test_remote_revision.c). This is how tests verify the two functions pair
 * up correctly: a commit followed by one simulated signal delivery must
 * consume exactly one suppression slot and leave the counter unchanged;
 * an unpaired simulated delivery (no preceding commit) must still bump the
 * counter normally. Always operates on the process-wide singleton (see
 * dt_remote_revision_test_singleton()). Not used by production code. */
void dt_remote_revision_test_simulate_history_change_signal(void);

/** Returns the process-wide singleton if dt_remote_revision_connect() has
 * been called and not yet matched by dt_remote_revision_disconnect(), or
 * NULL otherwise -- e.g. `security/enable_remote_control` is off, or in
 * unit tests, which never call connect() at all. This is how
 * remote_edit.c's read handlers reach the live revision without taking a
 * dependency on remote_server.h's session/server types. Note this is
 * about whether a server is *currently* connected, not whether the
 * counter has ever moved: the underlying storage persists across
 * disconnect()/connect() cycles even while this returns NULL. */
const dt_remote_revision_t *dt_remote_revision_current(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
