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
// (dt_dev_history_end survives across restarts via the database;
// this counter is process-local and resets to 0 every time darktable
// starts) and is never persisted.
//
// Ownership: the storage for the single, process-wide tracker instance
// lives on `dt_remote_server_t` (embedded field), created/connected in
// dt_remote_server_start() and disconnected/torn down in
// dt_remote_server_stop() -- see control/remote_server.c. This header
// intentionally has no dependency on remote_server.h/remote_protocol.h
// (gio, json-glib): remote_edit.c reaches the live tracker only through
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
 * one thread (i.e. before dt_remote_revision_connect()). */
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

/* ---------------------------------------------------------------------- */
/* signal wiring (main-thread only; not unit-tested -- see remote_server.c */
/* and the note in remote_revision.c; covered by later integration tests)  */
/* ---------------------------------------------------------------------- */

/** Connects `rev` to DT_SIGNAL_DEVELOP_HISTORY_CHANGE (bump) and
 * DT_SIGNAL_DEVELOP_IMAGE_CHANGED (bump + re-stamp imgid from
 * darktable.develop), and registers `rev` as the tracker
 * dt_remote_revision_current() returns. Call once, on the main context,
 * after the signal system and darktable.develop both exist -- i.e. from
 * dt_remote_server_start(), mirroring the connect pattern at
 * src/libs/history.c:165. */
void dt_remote_revision_connect(dt_remote_revision_t *rev);

/** Disconnects everything dt_remote_revision_connect() wired up, and
 * clears dt_remote_revision_current() if `rev` was the registered
 * tracker. NULL-safe. Call from dt_remote_server_stop(), before `rev`'s
 * storage is freed. */
void dt_remote_revision_disconnect(dt_remote_revision_t *rev);

/** Returns the tracker most recently passed to
 * dt_remote_revision_connect() (and not yet disconnected), or NULL if no
 * remote server is currently running -- e.g. `security/enable_remote_control`
 * is off, or in unit tests, which never call connect() at all. This is
 * how remote_edit.c's read handlers reach the live revision without
 * taking a dependency on remote_server.h's session/server types. */
const dt_remote_revision_t *dt_remote_revision_current(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
