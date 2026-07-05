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

#include "control/remote_frame.h"

#include <string.h>

G_DEFINE_QUARK(dt-remote-frame-error-quark, dt_remote_frame_error)

/* ---------------------------------------------------------------------- */
/* parser                                                                  */
/* ---------------------------------------------------------------------- */

void dt_remote_frame_parser_clear(dt_remote_frame_parser_t *p)
{
  if(!p) return;
  if(p->body) g_byte_array_unref(p->body);
  memset(p, 0, sizeof(*p));
}

gboolean dt_remote_frame_feed(dt_remote_frame_parser_t *p,
                              const uint8_t *data, size_t len,
                              dt_remote_frame_cb on_frame, gpointer ud,
                              GError **error)
{
  g_return_val_if_fail(p != NULL, FALSE);
  g_return_val_if_fail(data != NULL || len == 0, FALSE);
  g_return_val_if_fail(on_frame != NULL, FALSE);

  // feed() must not be re-entered from the on_frame callback: nested calls
  // interleave two byte streams through one parser state
  g_return_val_if_fail(!p->in_feed, FALSE);
  p->in_feed = TRUE;

  if(p->failed)
  {
    g_set_error(error, DT_REMOTE_FRAME_ERROR, DT_REMOTE_FRAME_ERROR_FAILED,
               "frame parser is in a failed state; discard the connection");
    p->in_feed = FALSE;
    return FALSE;
  }

  size_t pos = 0;
  while(pos < len)
  {
    // 1. fill the length header before anything else is allocated
    if(p->header_filled < sizeof(p->header))
    {
      size_t need = sizeof(p->header) - p->header_filled;
      size_t take = MIN(need, len - pos);
      memcpy(p->header + p->header_filled, data + pos, take);
      p->header_filled += take;
      pos += take;

      if(p->header_filled < sizeof(p->header))
        break;  // header still incomplete; wait for the next feed

      uint32_t net_len;
      memcpy(&net_len, p->header, sizeof(net_len));
      const uint32_t body_len = GUINT32_FROM_BE(net_len);

      if(body_len == 0)
      {
        p->failed = TRUE;
        g_set_error(error, DT_REMOTE_FRAME_ERROR, DT_REMOTE_FRAME_ERROR_ZERO_LENGTH,
                   "zero-length frame is not permitted");
        p->in_feed = FALSE;
        return FALSE;
      }
      if(body_len > DT_REMOTE_MAX_FRAME)
      {
        p->failed = TRUE;
        g_set_error(error, DT_REMOTE_FRAME_ERROR, DT_REMOTE_FRAME_ERROR_TOO_LARGE,
                   "frame length %u exceeds the %u byte maximum",
                   body_len, (unsigned)DT_REMOTE_MAX_FRAME);
        p->in_feed = FALSE;
        return FALSE;
      }

      // length validated -- only now does the body buffer get allocated
      p->body_expected = body_len;
      p->body = g_byte_array_sized_new(body_len);
    }

    // 2. accumulate the body
    const size_t need = p->body_expected - p->body->len;
    const size_t take = MIN(need, len - pos);
    if(take > 0)
    {
      g_byte_array_append(p->body, data + pos, take);
      pos += take;
    }

    if(p->body->len < p->body_expected)
      break;  // body still incomplete; wait for the next feed

    // 3. frame complete: reset parser state for the next frame *before*
    // invoking the callback, so a callback that inspects `p` sees an
    // idle parser, and so an early stop (on_frame returning FALSE)
    // leaves `p` idle too.
    GByteArray *finished = p->body;
    p->body = NULL;
    p->header_filled = 0;
    p->body_expected = 0;
    memset(p->header, 0, sizeof(p->header));

    GBytes *payload = g_byte_array_free_to_bytes(finished);  // takes ownership of finished's data
    const gboolean keep_going = on_frame(payload, ud);
    g_bytes_unref(payload);  // callback only borrowed it

    if(!keep_going)
    {
      p->in_feed = FALSE;
      return TRUE;  // caller-requested early stop; not an error
    }
  }

  p->in_feed = FALSE;
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* encoder                                                                 */
/* ---------------------------------------------------------------------- */

GBytes *dt_remote_frame_encode(GBytes *payload)
{
  if(!payload) return NULL;

  const gsize size = g_bytes_get_size(payload);
  if(size == 0 || size > DT_REMOTE_MAX_FRAME) return NULL;

  GByteArray *out = g_byte_array_sized_new(4 + size);

  uint8_t header[4];
  const uint32_t net_len = GUINT32_TO_BE((uint32_t)size);
  memcpy(header, &net_len, sizeof(net_len));
  g_byte_array_append(out, header, sizeof(header));

  gsize data_len = 0;
  gconstpointer data = g_bytes_get_data(payload, &data_len);
  g_byte_array_append(out, data, data_len);

  return g_byte_array_free_to_bytes(out);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
