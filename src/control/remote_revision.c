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

#include "control/remote_revision.h"

#include "control/signal.h"
#include "develop/develop.h"

// Guards `counter`/`imgid` on every dt_remote_revision_t instance. A
// single process-wide lock is deliberate and sufficient: there is at
// most one live tracker at a time (one remote server per process), and
// contention is negligible -- writers are two signal callbacks on the
// main context, the reader is the request handler on the same context;
// see the header comment for why GLib's 64-bit atomics are not used
// here (they require GLib >= 2.74; this tree caps GLIB_VERSION_MAX_ALLOWED
// at 2.56).
G_LOCK_DEFINE_STATIC(remote_revision);

void dt_remote_revision_init(dt_remote_revision_t *rev)
{
  if(!rev) return;
  rev->counter = 0;
  rev->imgid = NO_IMGID;
}

void dt_remote_revision_bump(dt_remote_revision_t *rev)
{
  if(!rev) return;
  G_LOCK(remote_revision);
  rev->counter++;  // unsigned: silently wraps on overflow, by design (see header)
  G_UNLOCK(remote_revision);
}

void dt_remote_revision_stamp_image(dt_remote_revision_t *rev, const dt_imgid_t imgid)
{
  if(!rev) return;
  G_LOCK(remote_revision);
  rev->counter++;
  rev->imgid = imgid;
  G_UNLOCK(remote_revision);
}

uint64_t dt_remote_revision_get(const dt_remote_revision_t *rev)
{
  if(!rev) return 0;
  G_LOCK(remote_revision);
  const uint64_t value = rev->counter;
  G_UNLOCK(remote_revision);
  return value;
}

dt_imgid_t dt_remote_revision_get_imgid(const dt_remote_revision_t *rev)
{
  if(!rev) return NO_IMGID;
  G_LOCK(remote_revision);
  const dt_imgid_t imgid = rev->imgid;
  G_UNLOCK(remote_revision);
  return imgid;
}

gboolean dt_remote_revision_matches(const dt_remote_revision_t *rev,
                                    const uint64_t expected_revision,
                                    const dt_imgid_t expected_imgid)
{
  if(!rev) return FALSE;
  G_LOCK(remote_revision);
  const uint64_t counter = rev->counter;
  const dt_imgid_t imgid = rev->imgid;
  G_UNLOCK(remote_revision);
  return counter == expected_revision && imgid == expected_imgid;
}

void dt_remote_revision_test_set_counter(dt_remote_revision_t *rev, const uint64_t value)
{
  if(!rev) return;
  G_LOCK(remote_revision);
  rev->counter = value;
  G_UNLOCK(remote_revision);
}

/* ---------------------------------------------------------------------- */
/* process-wide singleton                                                  */
/* ---------------------------------------------------------------------- */

// The one, process-lifetime instance of the tracker (file header's
// "Ownership" paragraph). Relies on C's static zero-initialization to
// start in exactly the state dt_remote_revision_init() would produce
// (counter 0, imgid NO_IMGID == 0) -- production code deliberately never
// calls dt_remote_revision_init() on this instance, since doing so on
// every dt_remote_server_start() is the bug this file fixes: the counter
// must survive a stop/start cycle within one process.
static dt_remote_revision_t _singleton;

// TRUE between a dt_remote_revision_connect() and its matching
// dt_remote_revision_disconnect() -- i.e. "a remote server is currently
// wired up to _singleton". Guarded by the same lock as _singleton's
// counter/imgid so connect()/disconnect() can be checked-and-set
// atomically; this is what makes both idempotent (a second connect()
// before a disconnect() does not double-subscribe the signal handlers,
// and a disconnect() with no matching connect() is a safe no-op) without
// relying on an undocumented "only ever called from the main context"
// convention for the flag itself. The signal (dis)connect calls
// themselves still must run on the main context, same as always.
static gboolean _connected = FALSE;

// Number of upcoming DEVELOP_HISTORY_CHANGE deliveries to treat as already
// accounted for (guarded by the same lock as _singleton/_connected). See
// dt_remote_revision_commit_history_change()'s header comment for why this
// exists: the signal is delivered asynchronously even from the main
// thread, so the mutation engine bumps synchronously itself and arms one
// slot here per commit, and _on_history_change() consumes (rather than
// double-counts) exactly that many redundant deliveries.
static guint _suppress_history_change = 0;

// Shared by the real signal callback and the test-only simulation seam
// (dt_remote_revision_test_simulate_history_change_signal()) so both run
// identical logic.
static void _history_change_bump_or_suppress(void)
{
  G_LOCK(remote_revision);
  if(_suppress_history_change > 0)
  {
    _suppress_history_change--;
    G_UNLOCK(remote_revision);
    return;
  }
  G_UNLOCK(remote_revision);
  dt_remote_revision_bump(&_singleton);
}

static void _on_history_change(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  _history_change_bump_or_suppress();
}

static void _on_image_changed(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  // darktable.develop is the only source of truth for "the current
  // image" -- read synchronously, on the same main context the signal
  // was raised from (internals §1).
  const dt_imgid_t imgid =
    (darktable.develop && dt_is_valid_imgid(darktable.develop->image_storage.id))
    ? darktable.develop->image_storage.id
    : NO_IMGID;
  dt_remote_revision_stamp_image(&_singleton, imgid);
}

void dt_remote_revision_connect(void)
{
  G_LOCK(remote_revision);
  const gboolean already_connected = _connected;
  _connected = TRUE;
  G_UNLOCK(remote_revision);
  if(already_connected) return;  // idempotent: never double-subscribe

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _on_history_change, NULL);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _on_image_changed, NULL);
}

void dt_remote_revision_disconnect(void)
{
  G_LOCK(remote_revision);
  const gboolean was_connected = _connected;
  _connected = FALSE;
  G_UNLOCK(remote_revision);
  if(!was_connected) return;  // idempotent: nothing to tear down

  DT_CONTROL_SIGNAL_DISCONNECT(_on_history_change, NULL);
  DT_CONTROL_SIGNAL_DISCONNECT(_on_image_changed, NULL);
}

const dt_remote_revision_t *dt_remote_revision_current(void)
{
  G_LOCK(remote_revision);
  const gboolean connected = _connected;
  G_UNLOCK(remote_revision);
  return connected ? &_singleton : NULL;
}

uint64_t dt_remote_revision_commit_history_change(void)
{
  // Deliberately unconditional (no `_connected` gate): this always
  // operates on the singleton's storage, same as the real
  // DEVELOP_HISTORY_CHANGE handler and the test-only simulation seam below
  // -- the file header's "Ownership" paragraph already establishes that
  // the singleton's counter is bumped independent of whether a server is
  // currently connected (see test_singleton_survives_disconnect_reconnect_
  // cycle in test_remote_revision.c, which bumps it directly while
  // disconnected). In production this is only ever reachable through the
  // live mutation engine, which itself is only reachable while a server
  // is connected, so the distinction is moot there; unconditional
  // behavior here is what keeps this function testable without requiring
  // a live darktable.signals connection.
  G_LOCK(remote_revision);
  _singleton.counter++;  // synchronous: see header comment for why
  _suppress_history_change++;
  const uint64_t value = _singleton.counter;
  G_UNLOCK(remote_revision);
  return value;
}

void dt_remote_revision_test_simulate_history_change_signal(void)
{
  _history_change_bump_or_suppress();
}

/* ---------------------------------------------------------------------- */
/* test-only singleton access (see remote_revision.h)                      */
/* ---------------------------------------------------------------------- */

dt_remote_revision_t *dt_remote_revision_test_singleton(void)
{
  return &_singleton;
}

void dt_remote_revision_test_reset_singleton(void)
{
  dt_remote_revision_init(&_singleton);
  G_LOCK(remote_revision);
  _connected = FALSE;
  _suppress_history_change = 0;
  G_UNLOCK(remote_revision);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
