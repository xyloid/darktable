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

// Authenticated loopback server for the darktable MCP remote-edit
// sidecar: binds 127.0.0.1:0 (never a wildcard interface), accepts
// connections asynchronously on the GLib main context, requires a valid
// `hello` as the first frame on every connection, and dispatches every
// later frame through remote_protocol.h. See the remote-edit internals
// design doc §1 (threading), §6 (session/pending types, normative) and
// §7 (server/crypto/discovery, normative).
//
// Threading: dt_remote_server_start()/dt_remote_server_stop() and every
// accept/read/write callback run on the GLib main context the server was
// started from -- in darktable that is the GTK main thread, which is
// also the only thread allowed to touch darktable.develop. This is load-
// bearing: request handlers run inline with no cross-thread marshalling
// (internals §1).
//
// Lifecycle: exactly one server per darktable process, owned by
// `darktable.remote_server` (src/common/darktable.h). Start it only
// after control/signals/config/GUI are all up (see src/common/
// darktable.c, near the end of dt_init()'s GUI branch); stop it before
// any GUI teardown, while darktable.develop is still valid (session
// handlers read it synchronously).
//
// Enablement: exactly one preference, `security/enable_remote_control`
// (data/darktableconfig.xml.in), default false. There is deliberately no
// second, parallel opt-in: integration tests enable the service by
// pointing darktable's existing `--configdir <dir>` (or `--conf
// security/enable_remote_control=true`) at a private, temporary
// directory whose darktablerc already sets that one preference to true.

#pragma once

#include "control/remote_frame.h"
#include "control/remote_protocol.h"

#include <gio/gio.h>
#include <glib.h>
#include <stddef.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* limits (internals §7, normative)                                        */
/* ---------------------------------------------------------------------- */

#define DT_REMOTE_SERVER_MAX_CONNECTIONS 4
#define DT_REMOTE_SERVER_MAX_PENDING 8
#define DT_REMOTE_SERVER_MAX_AUTH_FAILURES 3

/* ---------------------------------------------------------------------- */
/* session / pending types (internals §6, normative shape;                 */
/* this header is the one non-opaque definition -- remote_protocol.h only  */
/* forward-declares the same struct tags)                                  */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_server_t dt_remote_server_t;

typedef enum dt_remote_session_auth_state_t
{
  DT_REMOTE_SESSION_AWAIT_HELLO,
  DT_REMOTE_SESSION_READY,
} dt_remote_session_auth_state_t;

struct dt_remote_session_t   // one per accepted connection
{
  GSocketConnection *connection;          // ref held
  dt_remote_session_auth_state_t auth_state;
  int auth_failures;                      // closes at DT_REMOTE_SERVER_MAX_AUTH_FAILURES
  int pending_requests;                   // rejected busy at DT_REMOTE_SERVER_MAX_PENDING
  dt_remote_frame_parser_t frame;         // per-connection streaming parser
  dt_remote_server_t *server;             // back-pointer: limits, token, session list

  /* -- private I/O bookkeeping; not part of the normative shape above --
   *
   * Lifecycle: freeing a session while GIO still has an outstanding
   * async read/write pointing at it would be a use-after-free, so
   * freeing is deferred: `io_refs` counts in-flight async operations
   * (incremented right before each g_input_stream_read_async()/
   * g_output_stream_write_all_async() call, decremented at the top of
   * its completion callback); `closing` marks that the session should
   * be torn down as soon as `io_refs` reaches 0. Only `_on_read_ready`/
   * `_on_write_ready` (via `_session_after_io_completed()`) ever
   * actually free a session, so this is race-free on the single GLib
   * main-context thread these callbacks run on. */
  GInputStream *input;                    // borrowed from connection
  GOutputStream *output;                  // borrowed from connection
  GCancellable *io_cancellable;           // cancels in-flight *reads* on close (writes
                                          // are not individually cancellable -- see
                                          // remote_server.c for why: it lets a final
                                          // error response finish sending before close)
  guint8 read_buf[4096];
  GQueue *write_queue;                    // owned GBytes* (already framed), FIFO
  GBytes *inflight_write;                 // owned; the write currently in flight, if any
  gboolean writing;                       // a write is currently in flight
  gboolean closing;                       // logical close requested (see above)
  int io_refs;                            // in-flight async op count (see above);
                                          // includes one ref per live pending --
                                          // dt_remote_async_begin() takes it,
                                          // complete()/abort() release it -- so a
                                          // session with an in-flight background
                                          // job stays alive until the job's
                                          // main-thread completion has run: the
                                          // same free-deferral discipline as
                                          // reads/writes
  GPtrArray *pendings;                    // dt_remote_pending_t* currently in
                                          // flight; the array is owned here but
                                          // its elements are owned by the async
                                          // lifecycle below (never freed by the
                                          // array). Walked on close to fire each
                                          // pending's cancellable.
};

// Shutdown reclaim handshake for a queued background job (render_preview).
// Shared, atomically refcounted, and pointed at by BOTH the pending (main
// thread) and the queued job's params (worker thread), so either side can
// read/CAS the state without racing the other's free -- the state atom
// outlives whichever container is freed first. See remote_server.c.
//
// State machine (g_atomic CAS; exactly one transition wins):
//   QUEUED -> RUNNING     claimed by the job's worker (_preview_job_run)
//   QUEUED -> RECLAIMED   claimed by dt_remote_server_stop() at shutdown,
//                         for a job whose worker will never dequeue it
// The loser of the race must not touch the pending: if stop reclaimed, the
// worker frees only its own params; if the worker ran, stop leaves the
// pending for the job's completion to release.
typedef struct dt_remote_job_handshake_t dt_remote_job_handshake_t;

/** Allocates a handshake in the QUEUED state with refcount 1. */
dt_remote_job_handshake_t *dt_remote_job_handshake_new(void);
/** Adds a reference. NULL-safe. */
void dt_remote_job_handshake_ref(dt_remote_job_handshake_t *h);
/** Drops a reference, freeing at zero. NULL-safe. */
void dt_remote_job_handshake_unref(dt_remote_job_handshake_t *h);
/** Atomically claims QUEUED -> RUNNING; TRUE iff this call won. NULL -> FALSE. */
gboolean dt_remote_job_handshake_claim_run(dt_remote_job_handshake_t *h);
/** Atomically claims QUEUED -> RECLAIMED; TRUE iff this call won. NULL -> FALSE. */
gboolean dt_remote_job_handshake_reclaim(dt_remote_job_handshake_t *h);

struct dt_remote_pending_t   // one per in-flight async request
{
  dt_remote_session_t *session;   // connection ref held via session
  gint64 request_id;
  GCancellable *cancellable;      // fired on disconnect; job checks it
  dt_remote_job_handshake_t *handshake;  // nullable; non-NULL once a
                                         // reclaimable background job has been
                                         // queued for this pending (owned:
                                         // one ref, dropped when the pending
                                         // is released)
};

/* ---------------------------------------------------------------------- */
/* async request lifecycle (internals §6: "async methods return NULL from  */
/* the handler and complete later: the pending object holds the connection */
/* ref, request id, and a cancellable that fires if the connection drops   */
/* before the job completes"). Main-context only, all three: begin() runs  */
/* inline in dispatch; complete()/abort() run from a                       */
/* g_main_context_invoke()d job completion or from dispatch itself.        */
/* ---------------------------------------------------------------------- */

/** Registers one in-flight async request against `session`: allocates a
 * pending with a fresh cancellable (fired by
 * dt_remote_session_request_close() on disconnect), records it on the
 * session's pending list, and takes one io_refs liveness ref so the
 * session cannot be freed before dt_remote_async_complete()/_abort()
 * releases the pending -- which is what makes the g_main_context_invoke()d
 * completion of a background job safe against use-after-free by
 * construction. Returns NULL (registering nothing) if `session` is NULL
 * (e.g. a dispatcher unit test with no transport; the dispatcher maps
 * that to a synchronous internal error). The returned pending is owned by
 * the async lifecycle: exactly one complete()/abort() call must
 * eventually follow. */
dt_remote_pending_t *dt_remote_async_begin(dt_remote_session_t *session, gint64 request_id);

/** Completes one deferred request. If `response` is non-NULL and the
 * session is not closing, sends it through the normal session write path
 * (which includes the oversized-frame -> request_too_large fallback); a
 * NULL `response` -- the cancelled/disconnected path -- sends nothing.
 * Either way: takes ownership of `response`, decrements the session's
 * pending_requests count (releasing the increment _handle_ready_frame
 * made before the handler deferred), releases the liveness ref taken by
 * dt_remote_async_begin(), and frees the pending -- which, on a closing
 * session whose other in-flight ops have already drained, is the point
 * the session itself is finally freed. NULL-safe on `pending` (releases
 * `response` and returns). */
void dt_remote_async_complete(dt_remote_pending_t *pending, JsonNode *response);

/** Releases a pending whose request resolved synchronously after all
 * (dispatch produced an immediate validation-error response instead of
 * deferring): frees the pending and releases the liveness ref WITHOUT
 * touching pending_requests -- for a synchronous response the transport
 * layer's own "if(response) pending_requests--" still runs, so
 * decrementing here too would double-count. NULL-safe. */
void dt_remote_async_abort(dt_remote_pending_t *pending);

/** Idempotently requests teardown of `session`: marks it closing, cancels
 * its in-flight read, and fires every live pending's cancellable so
 * in-flight background jobs bail out early and their completions release
 * their result buffers without writing to the closing session. Never
 * frees `session` itself -- that happens once every in-flight op (reads,
 * writes, pendings) has released its ref. Main-context only. Public
 * (rather than file-static) so unit tests can drive the
 * cancellation-on-disconnect contract against a fake session; production
 * callers are all inside remote_server.c. NULL-safe. */
void dt_remote_session_request_close(dt_remote_session_t *session);

/* ---------------------------------------------------------------------- */
/* lifecycle                                                                */
/* ---------------------------------------------------------------------- */

/** Starts the server if `security/enable_remote_control` is true in the
 * active configuration; otherwise returns NULL immediately (disabled is
 * not an error). On enablement, generates a 32-byte token
 * (dt_crypto_random_bytes), binds `127.0.0.1:0` (loopback only -- never
 * a wildcard interface), and writes the discovery record. Every failure
 * (bind, discovery write, ...) is logged with the token redacted and
 * returns NULL -- never aborts the caller. Call once, from the GLib
 * main context darktable.c wants the server to run on, after control/
 * signals/config/GUI are ready. Also connects the process-wide revision
 * tracker (internals §5; owned by control/remote_revision.c, and
 * outliving this server object -- see the "Ownership" note in
 * control/remote_revision.h) to
 * DT_SIGNAL_DEVELOP_HISTORY_CHANGE/DT_SIGNAL_DEVELOP_IMAGE_CHANGED;
 * remote_edit.c reads it via dt_remote_revision_current(), not through
 * this server object. Connecting never resets the tracker's counter --
 * it survives a stop()/start() cycle within one process by design. */
dt_remote_server_t *dt_remote_server_start(void);

/** Stops `server`: disconnects the revision tracker (its counter is
 * untouched -- see dt_remote_server_start()'s comment), closes the
 * listener (no further connections accepted), drops every live session
 * (cancels in-flight I/O, unrefs
 * connections), deletes the discovery record, and frees all state.
 * NULL-safe. Must run on the same main context/thread `server` was
 * started from, before any GUI teardown that would invalidate
 * darktable.develop (session handlers read it synchronously). */
void dt_remote_server_stop(dt_remote_server_t *server);

/** returns the OS-assigned loopback port `server` is listening on, or 0
 * if `server` is NULL. Exposed for tests/tooling; production code has
 * no need for it once the discovery record is written. */
guint16 dt_remote_server_get_port(const dt_remote_server_t *server);

/* ---------------------------------------------------------------------- */
/* pure helpers exposed for unit tests (no sockets involved)               */
/* ---------------------------------------------------------------------- */

/** constant-time comparison used for the `hello` token check: always
 * walks every byte of both inputs via a fixed accumulator with no early
 * exit on a mismatched byte position (per internals §7: "fixed-length
 * memcmp over the full length regardless of mismatch position"). The one
 * exception is a length mismatch, checked up front -- token length is
 * already visible to anyone who can see the frame's byte length on the
 * wire, so this does not leak anything beyond what is already inevitable.
 * `alen == 0 && blen == 0` is trivially TRUE without dereferencing `a`/`b`
 * (either may be NULL in that case). */
gboolean dt_remote_constant_time_equal(const char *a, size_t alen,
                                       const char *b, size_t blen);

/** base64url-encodes (RFC 4648 §5: '+'/'/' -> '-'/'_', no padding)
 * `len` bytes at `data`. Returns a newly allocated, NUL-terminated
 * string owned by the caller (g_free), or NULL if `data` is NULL while
 * `len != 0`. Used to encode the session token for the wire/discovery
 * record. */
char *dt_remote_base64url_encode(const guint8 *data, size_t len);

/** builds a one-line, log-safe description of an authentication failure
 * (failure count only). Deliberately accepts `attempted_token` as a
 * parameter -- so a future edit that starts interpolating it into this
 * string is structurally caught by the unit test that asserts the
 * returned string never contains the token it was given -- but the
 * returned string never actually contains it. Owned by caller, g_free. */
char *dt_remote_server_describe_auth_failure_for_log(int auth_failures,
                                                     const char *attempted_token);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
