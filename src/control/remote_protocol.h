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
#include "control/remote_scopes.h"

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

// The remote_edit entry points need loaded module metadata or a live
// dt_develop_t to do anything interesting, which unit tests do not have.
// Handlers call through this small function-pointer table instead of the
// dt_remote_* symbols directly, so tests can substitute canned results and
// exercise the dispatcher (JSON validation, envelope shape, error mapping)
// without a running darktable. Production code never touches this -- the
// table defaults to the real dt_remote_* functions.
typedef struct dt_remote_protocol_calls_t
{
  gboolean (*get_state)(dt_remote_state_t **out, dt_remote_error_t **error);
  gboolean (*list_modules)(GPtrArray **out, dt_remote_error_t **error);
  gboolean (*get_module_schema)(const char *op, dt_remote_module_schema_t **out,
                                dt_remote_error_t **error);
  gboolean (*get_module_primitive_schema)(const char *op, dt_remote_module_schema_t **out,
                                          dt_remote_error_t **error);
  gboolean (*get_module_params)(const dt_remote_module_ref_t *ref, GPtrArray **out,
                                GHashTable **semantic_out, dt_remote_error_t **error);
  gboolean (*set_module_params)(const dt_remote_module_ref_t *ref, const dt_remote_patch_t *patch,
                                const uint64_t *expected_revision, dt_remote_mutation_result_t **out,
                                dt_remote_error_t **error);
  gboolean (*set_module_enabled)(const dt_remote_module_ref_t *ref, gboolean enabled,
                                 const uint64_t *expected_revision, dt_remote_mutation_result_t **out,
                                 dt_remote_error_t **error);
  gboolean (*reset_module)(const dt_remote_module_ref_t *ref, const uint64_t *expected_revision,
                           dt_remote_mutation_result_t **out, dt_remote_error_t **error);
  gboolean (*create_module_instance)(const dt_remote_module_ref_t *ref, gboolean copy_params,
                                     const uint64_t *expected_revision,
                                     dt_remote_mutation_result_t **out, dt_remote_error_t **error);
  gboolean (*get_history)(int limit, GPtrArray **out, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*undo)(uint64_t expected_revision, uint64_t *revision, dt_remote_error_t **error);
  gboolean (*render_preview_prepare)(dt_remote_preview_request_t *out,
                                     const char *show_mask_op,
                                     int show_mask_instance,
                                     dt_remote_error_t **error);
  // Live revision read at preview-completion time (internals §8 coherence
  // check): if it differs from the revision the preview was stamped with,
  // darkroom state drifted during the render and the result is discarded.
  uint64_t (*current_revision)(void);
  // compute_scopes main-thread precondition (not_in_darkroom/no_image_open).
  gboolean (*scopes_prepare)(dt_remote_error_t **error);
  // Tier-1 blend surface (blend_params capability): live-lookup JSON
  // builders; NULL-returning (never erroring) -- the handlers omit the
  // member. A NULL function pointer (older test call tables) also omits.
  JsonNode *(*blend_schema)(const dt_remote_module_ref_t *ref);
  JsonNode *(*blend_read)(const dt_remote_module_ref_t *ref);
} dt_remote_protocol_calls_t;

/** overrides the remote-edit call table (test seam only). Pass NULL to
 * restore the real dt_remote_* implementations. Not thread-safe; call
 * only from a single-threaded test process, before dispatching. */
void dt_remote_protocol_set_calls(const dt_remote_protocol_calls_t *calls);

/* ---------------------------------------------------------------------- */
/* test seam: overridable async transport table                            */
/* ---------------------------------------------------------------------- */

// The async half of an async method (render_preview so far) crosses into
// remote_server's pending lifecycle and darktable's background job queue
// -- neither exists in a dispatcher unit test. Same call-table idiom as
// dt_remote_protocol_calls_t: production defaults are the real
// dt_remote_async_* functions (remote_server.c) plus this file's own
// DT_JOB_QUEUE_SYSTEM_BG job for queue_preview; tests substitute fakes
// that record the begin/queue/abort traffic.
typedef struct dt_remote_protocol_async_t
{
  dt_remote_pending_t *(*begin)(dt_remote_session_t *session, gint64 request_id);
  void (*abort)(dt_remote_pending_t *pending);
  void (*complete)(dt_remote_pending_t *pending, JsonNode *response);
  gboolean (*queue_preview)(dt_remote_pending_t *pending,
                            const dt_remote_preview_request_t *req,
                            int max_px, int quality);
  // compute_scopes' second async method reuses the same begin/abort/
  // complete lifecycle and the same DT_JOB_QUEUE_SYSTEM_BG + reclaim
  // handshake machinery as queue_preview -- only the job payload differs.
  gboolean (*queue_scopes)(dt_remote_pending_t *pending,
                           const dt_remote_scopes_request_t *req);
} dt_remote_protocol_async_t;

/** overrides the async transport table (test seam only). Pass NULL to
 * restore the production implementations. Same caveats as
 * dt_remote_protocol_set_calls(). */
void dt_remote_protocol_set_async(const dt_remote_protocol_async_t *ops);

/* ---------------------------------------------------------------------- */
/* render_preview response shaping (pure; exposed for unit tests)          */
/* ---------------------------------------------------------------------- */

/** TRUE iff a JPEG of `jpeg_len` bytes still fits one wire frame after
 * base64 inflation (4 * ceil(n/3)) plus envelope headroom -- the
 * proactive check behind the wire contract's request_too_large error
 * ("result exceeds frame cap -- client retries with smaller max_px"),
 * preferred over relying on the transport's over-cap fallback because it
 * costs nothing and skips a pointless multi-MiB base64 encode. */
gboolean dt_remote_protocol_preview_fits_frame(size_t jpeg_len);

/** Builds render_preview's complete wire response for `request_id`:
 * exactly one of `preview`/`error` should be non-NULL (both borrowed).
 * A preview that fits the frame becomes the success envelope
 * {mime_type:"image/jpeg", width, height, revision, data:<base64>}; an
 * over-cap preview becomes the request_too_large error envelope
 * (retryable false, per the protocol reference's retryable table); an
 * `error` maps through the standard error envelope (preview_failed is
 * retryable). Both NULL degrades to an internal-error envelope rather
 * than crashing. Pure: unit-testable against the wire fixtures. */
JsonNode *dt_remote_protocol_build_preview_response(gint64 request_id,
                                                    const dt_remote_preview_t *preview,
                                                    const dt_remote_error_t *error);

/** The main-thread completion of a render job (invoked via
 * g_main_context_invoke() by the production job, or directly by tests):
 * takes ownership of `preview` and `error`. If the pending's cancellable
 * has fired (disconnect/close won the race), releases both buffers and
 * completes the pending with no response -- nothing is written to the
 * closing session; otherwise builds the response (see
 * dt_remote_protocol_build_preview_response(); a never-ran job with both
 * NULL becomes a retryable preview_failed) and completes the pending
 * with it. On the success path it first re-observes the live revision
 * (calls-table current_revision) and, if it drifted from the revision the
 * preview was stamped with, discards the payload and fails with a retryable
 * preview_failed (internals §8 coherence check). Completion always goes
 * through the async transport table, so the pending is released exactly
 * once either way. */
void dt_remote_protocol_finish_preview(dt_remote_pending_t *pending,
                                       dt_remote_preview_t *preview,
                                       dt_remote_error_t *error);

/* ---------------------------------------------------------------------- */
/* compute_scopes response shaping (pure; exposed for unit tests)          */
/* ---------------------------------------------------------------------- */

/** Builds compute_scopes' complete wire response for `request_id`:
 * exactly one of `result`/`error` should be non-NULL (both borrowed). A
 * `result` becomes the success envelope carrying top-level `revision`,
 * `source`, `color_profile`, `roi` and one key per requested scope
 * (histogram numeric summary and/or normalized bins; waveform/parade/
 * vectorscope PNG image objects {mime_type,width,height,data}); if the
 * composed response would exceed the frame cap it degrades to
 * request_too_large (retryable false). An `error` maps through the
 * standard error envelope (scope_failed is retryable). Both NULL degrades
 * to an internal-error envelope. Pure: unit-testable against fixtures. */
JsonNode *dt_remote_protocol_build_scopes_response(gint64 request_id,
                                                   const dt_remote_scopes_result_t *result,
                                                   const dt_remote_error_t *error);

/** The main-thread completion of a scopes job (invoked via the idle-source
 * dispatch, or directly by tests): takes ownership of `result` and
 * `error`. If the pending's cancellable has fired, releases both and
 * completes with no response; otherwise builds the response and completes.
 * Unlike render_preview there is NO completion-time revision drift check:
 * every scope derives from one captured buffer and stays valid for its
 * stamped revision by construction (design spec: "If darkroom state
 * changes during computation, the result remains valid for its reported
 * revision"). Completion always goes through the async transport table, so
 * the pending is released exactly once. */
void dt_remote_protocol_finish_scopes(dt_remote_pending_t *pending,
                                      dt_remote_scopes_result_t *result,
                                      dt_remote_error_t *error);

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
 * handler deferred completion (render_preview's handler defers once it
 * has validated params and queued the render job; the job's main-thread
 * completion sends the deferred response through
 * dt_remote_async_complete()); every other outcome, including all
 * malformed-input cases, returns a non-NULL success or error envelope.
 * Never crashes on malformed input. For an async method, dispatch
 * registers the pending (async table `begin`) before running the handler
 * -- the handler cannot, as it never sees the request id -- and releases
 * it (`abort`) if the handler resolves synchronously after all.
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
