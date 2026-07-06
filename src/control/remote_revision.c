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
/* signal wiring                                                           */
/* ---------------------------------------------------------------------- */

// The single process-wide "currently running server's tracker", set by
// dt_remote_revision_connect() and cleared by dt_remote_revision_disconnect().
// Only ever touched on the main context (connect/disconnect run there,
// same as everything else in this subsystem), so it needs no lock of its
// own -- unlike the counter/imgid pair above, which a background job's
// completion path could in principle read from off the main thread.
static dt_remote_revision_t *_active_revision = NULL;

static void _on_history_change(gpointer instance, gpointer user_data)
{
  (void)instance;
  dt_remote_revision_bump((dt_remote_revision_t *)user_data);
}

static void _on_image_changed(gpointer instance, gpointer user_data)
{
  (void)instance;
  // darktable.develop is the only source of truth for "the current
  // image" -- read synchronously, on the same main context the signal
  // was raised from (internals §1).
  const dt_imgid_t imgid =
    (darktable.develop && dt_is_valid_imgid(darktable.develop->image_storage.id))
    ? darktable.develop->image_storage.id
    : NO_IMGID;
  dt_remote_revision_stamp_image((dt_remote_revision_t *)user_data, imgid);
}

void dt_remote_revision_connect(dt_remote_revision_t *rev)
{
  if(!rev) return;
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _on_history_change, rev);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _on_image_changed, rev);
  _active_revision = rev;
}

void dt_remote_revision_disconnect(dt_remote_revision_t *rev)
{
  if(!rev) return;
  DT_CONTROL_SIGNAL_DISCONNECT(_on_history_change, rev);
  DT_CONTROL_SIGNAL_DISCONNECT(_on_image_changed, rev);
  if(_active_revision == rev) _active_revision = NULL;
}

const dt_remote_revision_t *dt_remote_revision_current(void)
{
  return _active_revision;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
