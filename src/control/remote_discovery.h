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

// Discovery record for the darktable MCP remote-edit sidecar: writes/
// removes `<configdir>/mcp/session-<pid>.json`, per the design doc's
// "Server lifecycle and discovery" section and the remote-edit internals
// doc §7.
//
// `config_dir` is taken as an explicit parameter rather than calling
// dt_loc_get_user_config_dir() internally, so this file has no darktable
// init dependency and unit tests can point it at a scratch directory.
// The one production caller (remote_server.c) is the one that calls
// dt_loc_get_user_config_dir() and passes the result in.
//
// Threading: no darktable state touched; call from wherever the server
// lifecycle (start/stop) runs (the GTK main thread in practice).

#pragma once

#include <glib.h>
#include <inttypes.h>

G_BEGIN_DECLS

/** writes the discovery record for this darktable process to
 * `<config_dir>/mcp/session-<pid>.json`: creates the `mcp` subdirectory
 * if needed, writes the record to a temporary sibling file, flushes and
 * closes it, restricts it to user-only read/write (attempted both at
 * creation and via an explicit chmod -- best-effort where the platform
 * has no such concept), and only then atomically renames it into place
 * -- so a concurrent reader can never observe a partially written record
 * or one with looser permissions than its final name ever had. Tolerates
 * unrelated stale files already present in `<config_dir>/mcp` (leftovers
 * from a crashed process with a different pid) -- only ever touches its
 * own `session-<pid>.json`.
 *
 * `token_b64` is embedded verbatim (already base64url-encoded) and is
 * the only field of the record covered by the "never log the token"
 * rule -- this function itself never logs it, only redacted diagnostics
 * (path, port, pid).
 *
 * Returns the full path to the written record (owned, g_free) on
 * success, or NULL on any failure (directory could not be created, temp
 * file could not be written/renamed, ...); every failure is logged,
 * redacted, and never aborts the caller. */
char *dt_remote_discovery_write(const char *config_dir, int port,
                                const char *token_b64,
                                const char *darktable_version);

/** deletes the discovery record at `path` (as returned by a prior
 * dt_remote_discovery_write() call) during orderly shutdown. NULL-safe;
 * tolerates the file already being gone (stale-record tolerance is
 * symmetric: we neither choke on other processes' leftovers nor on our
 * own record having already vanished). */
void dt_remote_discovery_remove(const char *path);

/* ---------------------------------------------------------------------- */
/* pure helper exposed for unit tests (log redaction)                      */
/* ---------------------------------------------------------------------- */

/** builds a one-line, log-safe description of a record about to be
 * written at `path` (port/pid only). Deliberately accepts `token_b64` as
 * a parameter -- so that a future edit which starts interpolating it
 * into this string is structurally caught by the unit test that asserts
 * the returned string never contains the token it was given -- but the
 * returned string never actually contains it. Owned by caller, g_free.
 * `dt_remote_discovery_write()` uses this for its own success/failure
 * diagnostics. */
char *dt_remote_discovery_describe_for_log(const char *path, int port,
                                           const char *token_b64);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
