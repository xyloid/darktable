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

// JSON dispatch for the darktable MCP remote-edit sidecar: takes one
// parsed request object and produces one response node, per the wire
// contract in docs/superpowers/specs/2026-07-05-darktable-mcp-protocol-
// reference.md. Owns request-ID validation, method lookup through a
// static allowlist, parameter shape/length checks, JSON<->remote-edit
// type conversion, the stable error envelope, and capability reporting.
//
// Does NOT own: authentication, framing, sockets, or GUI-thread
// scheduling -- those land with remote_server/remote_frame in later
// steps. `dt_remote_session_t`/`dt_remote_pending_t` are defined there;
// here they are opaque forward declarations threaded through the
// handler signature so later steps only add allowlist rows, not change
// this signature.
//
// Threading: like remote_edit, must be called from the GTK main thread
// once real (non-stubbed) remote-edit calls are wired in -- see the
// internals doc §1.

#pragma once

#include "control/remote_edit.h"

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* protocol version negotiated in `hello`; bump on any breaking wire change */
#define DT_REMOTE_PROTOCOL_VERSION 1

/* ---------------------------------------------------------------------- */
/* opaque transport types -- owned by remote_server, defined in a later    */
/* step; treated as opaque pointers here.                                  */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_session_t dt_remote_session_t;
typedef struct dt_remote_pending_t dt_remote_pending_t;

/* ---------------------------------------------------------------------- */
/* method allowlist (internals §6, normative)                              */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_method_t
{
  const char *name;
  gboolean needs_darkroom;   // metadata only: the precondition is enforced
                             // inside the remote_edit functions (plan step 1
                             // mandates the checks there); dispatch does not
                             // duplicate it
  gboolean is_mutation;      // serialized; rejected while another runs
  gboolean is_async;         // render_preview, compute_scopes
  JsonNode *(*handler)(JsonObject *params, dt_remote_session_t *session,
                       dt_remote_pending_t *pending /* async only */);
} dt_remote_method_t;

/** looks up a method by name in the static allowlist. Returns NULL if
 * `name` is not a known method. Exposed mainly for tests that want to
 * assert on the per-method flags without going through full dispatch. */
const dt_remote_method_t *dt_remote_protocol_lookup_method(const char *name);

/* ---------------------------------------------------------------------- */
/* test seam: overridable remote-edit call table                           */
/* ---------------------------------------------------------------------- */

// The four read-only remote_edit entry points need a live dt_develop_t to
// do anything interesting, which unit tests do not have. Handlers call
// through this small function-pointer table instead of the dt_remote_*
// symbols directly, so tests can substitute canned results and exercise
// the dispatcher (JSON validation, envelope shape, error mapping) without
// a running darktable. Production code never touches this -- the table
// defaults to the real dt_remote_* functions.
typedef struct dt_remote_protocol_calls_t
{
  gboolean (*get_state)(dt_remote_state_t **out, dt_remote_error_t **error);
  gboolean (*list_modules)(GPtrArray **out, dt_remote_error_t **error);
  gboolean (*get_module_schema)(const char *op, dt_remote_module_schema_t **out,
                                dt_remote_error_t **error);
  gboolean (*get_module_params)(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                dt_remote_error_t **error);
} dt_remote_protocol_calls_t;

/** overrides the remote-edit call table (test seam only). Pass NULL to
 * restore the real dt_remote_* implementations. Not thread-safe; call
 * only from a single-threaded test process, before dispatching. */
void dt_remote_protocol_set_calls(const dt_remote_protocol_calls_t *calls);

/* ---------------------------------------------------------------------- */
/* dispatch entry point                                                    */
/* ---------------------------------------------------------------------- */

/** validates and dispatches one already-parsed JSON request object,
 * returning one response node ready to serialize onto the wire.
 *
 * Handles, before any handler runs: request-id validation (present,
 * non-negative integer -- a request whose id cannot be determined gets
 * `"id": null` in the response, since there is nothing valid to echo),
 * method validation (present, string, known), and extracts `params`
 * (NULL if absent or not a JSON object -- each handler decides whether
 * that is acceptable for it). Per-field parameter validation (required/
 * optional, type, range, strict unknown-key rejection) is hand-rolled
 * per handler, per the internals doc.
 *
 * Returns NULL only when the resolved method is asynchronous and its
 * handler deferred completion (not reachable yet -- no async method has
 * a handler in this step); every other outcome, including all malformed-
 * input cases, returns a non-NULL success or error envelope. Never
 * crashes on malformed input.
 *
 * `session` may be NULL: no method implemented so far touches it (hello/
 * auth semantics land with remote_server). Ownership of the returned
 * node is transferred to the caller (`json_node_unref()` when done). */
JsonNode *dt_remote_protocol_dispatch(JsonObject *request, dt_remote_session_t *session);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
