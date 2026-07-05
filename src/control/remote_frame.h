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

// Wire framing for the darktable MCP remote-edit sidecar: a pure,
// streaming length-prefixed frame parser plus its matching encoder. A
// frame on the wire is a 4-byte big-endian payload length followed by
// exactly that many payload bytes. No JSON, no sockets, no GLib I/O --
// bytes go in, complete payloads come out -- so this file can be linked
// and fuzzed standalone (see the remote-edit internals design doc §4).
//
// Threading: none of these functions touch darktable state; call from
// whatever thread owns the connection's byte stream (the socket-server
// step reads on its own I/O thread and hands bytes to dt_remote_frame_feed
// there, then dispatches parsed payloads to the GTK main thread).

#pragma once

#include <glib.h>
#include <inttypes.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* limits                                                                  */
/* ---------------------------------------------------------------------- */

// Maximum payload size accepted by dt_remote_frame_feed()/produced by
// dt_remote_frame_encode(), in bytes. Enforced against the 4-byte length
// header *before* any body buffer is allocated, so a hostile or corrupt
// length prefix cannot be used to force an oversized allocation.
#define DT_REMOTE_MAX_FRAME ((uint32_t)(16 * 1024 * 1024))

/* ---------------------------------------------------------------------- */
/* errors                                                                  */
/* ---------------------------------------------------------------------- */

#define DT_REMOTE_FRAME_ERROR (dt_remote_frame_error_quark())
GQuark dt_remote_frame_error_quark(void);

typedef enum dt_remote_frame_error_code_t
{
  DT_REMOTE_FRAME_ERROR_ZERO_LENGTH,  // header declared a 0-byte payload
  DT_REMOTE_FRAME_ERROR_TOO_LARGE,    // header declared > DT_REMOTE_MAX_FRAME
  DT_REMOTE_FRAME_ERROR_FAILED,       // parser already failed; feed again
                                     // without resetting it
} dt_remote_frame_error_code_t;

/* ---------------------------------------------------------------------- */
/* streaming parser                                                        */
/* ---------------------------------------------------------------------- */

// Plain data: zero-initialize (e.g. `= {0}` or g_malloc0) to get a fresh
// parser ready for the start of a frame. Every field is meaningful to a
// caller inspecting parser state directly -- e.g. after a connection
// closes, `header_filled != 0 || body != NULL` means the peer hung up
// mid-frame, which is a protocol error the caller must report itself
// (this file has no notion of "connection closed").
typedef struct dt_remote_frame_parser_t
{
  uint8_t header[4];
  size_t header_filled;
  GByteArray *body;        // allocated only after header validates
  uint32_t body_expected;
  gboolean failed;
} dt_remote_frame_parser_t;

/** releases any body buffer the parser is holding and resets it to the
 * same state as a zero-initialized parser. Safe to call on an already-
 * clear or already-failed parser. Does not free `p` itself. NULL-safe. */
void dt_remote_frame_parser_clear(dt_remote_frame_parser_t *p);

// Called once per complete frame extracted from the stream, with a
// *borrowed* reference to its payload: valid only for the duration of
// the call. Call g_bytes_ref() if the payload needs to outlive the
// callback (e.g. queued for async dispatch). Return TRUE to keep
// extracting any further complete frames already present in the current
// dt_remote_frame_feed() call; returning FALSE stops extraction early for
// this call (any remaining unconsumed bytes in `data` are discarded, not
// buffered -- a caller that returns FALSE must be prepared for that).
typedef gboolean (*dt_remote_frame_cb)(GBytes *payload, gpointer user_data);

/** feeds `len` newly-arrived bytes into the parser, invoking `on_frame`
 * (required, non-NULL) once per complete frame found (header split
 * across calls, multiple frames in one call, and a body split across
 * calls are all handled -- callers may feed any chunking, down to one
 * byte at a time, and must get identical results). `data` may be NULL
 * iff `len` is 0.
 *
 * A declared payload length of 0 or greater than DT_REMOTE_MAX_FRAME is
 * a protocol error: `p->failed` is set, no body buffer is allocated, and
 * this call returns FALSE with `*error` set. Once `p->failed` is set,
 * every subsequent call returns FALSE immediately (with
 * DT_REMOTE_FRAME_ERROR_FAILED) until the caller discards the parser (or
 * resets it with dt_remote_frame_parser_clear(), which is only correct
 * to do if the caller is also discarding the underlying byte stream,
 * e.g. closing the connection) -- feed() never tries to resynchronize on
 * a corrupt header.
 *
 * Returns TRUE on success (including "consumed some header/body bytes,
 * still waiting for more" -- that is not an error, just incompleteness).
 */
gboolean dt_remote_frame_feed(dt_remote_frame_parser_t *p,
                              const uint8_t *data, size_t len,
                              dt_remote_frame_cb on_frame, gpointer ud,
                              GError **error);

/* ---------------------------------------------------------------------- */
/* encoder                                                                 */
/* ---------------------------------------------------------------------- */

/** encodes `payload` as one wire frame: a 4-byte big-endian length
 * followed by the payload bytes, in a single newly-allocated GBytes
 * owned by the caller. `payload` is not consumed (its own reference is
 * untouched). Returns NULL, allocating nothing, if `payload` is NULL,
 * empty (frame parsers reject zero-length frames, so this never
 * produces one), or larger than DT_REMOTE_MAX_FRAME. */
GBytes *dt_remote_frame_encode(GBytes *payload);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
