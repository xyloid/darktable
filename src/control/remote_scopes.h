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

// Remote-edit scope capture + computation service (plan step 10,
// internals §9). The GUI scopes lib pushes a copy of every final-preview
// buffer it processes (already converted to the histogram profile, on the
// preview-pixelpipe worker thread) into a single mutex-guarded slot,
// stamped with the process-local revision read at push time. The
// asynchronous compute_scopes handler snapshots that slot on a background
// job and runs the requested shared kernels (src/common/scopes.h) over the
// one retained buffer, so every scope in one response derives from one
// buffer and carries one revision by construction -- exactly what the
// protocol reference requires. No compute ever runs on the GUI thread.

#pragma once

#include "common/iop_profile.h"
#include "common/scopes.h"
#include "control/remote_edit.h"   // dt_remote_error_t

#include <gio/gio.h>               // GCancellable
#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* capture (producer side: the GUI scopes lib on the pixelpipe worker)     */
/* ---------------------------------------------------------------------- */

/** Pushes a copy of the latest final-preview buffer into the capture slot,
 * replacing any previous one. `hist_rgb` is 4*width*height interleaved
 * floats already in the histogram profile `vs_prof` (the same buffer the
 * GUI panel computes over); a deep copy is taken under the slot mutex, the
 * display-gamma (HLG Rec2020) LUT is resolved and copied, and the revision
 * is stamped from the thread-safe revision read. Runs on the preview
 * pixelpipe worker thread; NULL `hist_rgb` clears the slot. */
void dt_remote_scopes_push(const float *hist_rgb, int width, int height,
                           const dt_iop_order_iccprofile_info_t *vs_prof);

/** Clears and frees the capture slot (called at server stop). NULL-safe. */
void dt_remote_scopes_capture_reset(void);

/* ---------------------------------------------------------------------- */
/* request / result (consumer side: the compute_scopes background job)     */
/* ---------------------------------------------------------------------- */

typedef struct dt_remote_scopes_request_t
{
  gboolean want_histogram, want_waveform, want_parade, want_vectorscope;
  gboolean include_summary, include_bins, include_images;
  int image_size;      // clamped [128, 1024] by the handler
} dt_remote_scopes_request_t;

typedef struct dt_remote_scopes_image_t
{
  gboolean present;
  uint8_t *png;        // owned (g_free); PNG bytes
  size_t png_len;
  int width, height;
} dt_remote_scopes_image_t;

typedef struct dt_remote_scopes_result_t
{
  uint64_t revision;
  char *source;          // owned; "final_preview"
  char *color_profile;   // owned; printable histogram-profile name
  char *roi;             // owned; "full_image"

  gboolean has_histogram;
  gboolean has_histogram_summary;
  dt_scopes_histogram_summary_t histogram_summary;
  gboolean has_histogram_bins;
  float histogram_bins[3][DT_SCOPES_HISTOGRAM_BINS];  // normalized [0,1]

  dt_remote_scopes_image_t waveform;
  dt_remote_scopes_image_t parade;
  dt_remote_scopes_image_t vectorscope;
} dt_remote_scopes_result_t;

/** Frees a result and all owned buffers. NULL-safe. */
void dt_remote_scopes_result_free(dt_remote_scopes_result_t *result);

/** Main-thread precondition check for compute_scopes: fails with
 * DT_REMOTE_ERR_NOT_IN_DARKROOM / DT_REMOTE_ERR_NO_IMAGE_OPEN exactly like
 * the other darkroom methods. Must run on the GTK main thread, before
 * queueing the background job (mirrors dt_remote_render_preview_prepare's
 * precondition role, minus the history flush -- scopes read the captured
 * buffer, not the DB). */
gboolean dt_remote_scopes_prepare(dt_remote_error_t **error);

/** Background half of compute_scopes: snapshots the capture slot and runs
 * the requested shared kernels over the retained buffer. Returns FALSE
 * with DT_REMOTE_ERR_SCOPE_FAILED (retryable) if the slot is empty (fresh
 * darkroom, no preview run yet) or a kernel/encode fails, or if
 * `cancellable` has fired. On success `*out` carries the revision stamped
 * at capture and every requested scope's data. Must NOT run on the GUI
 * thread (it may run kernels for tens of ms); it touches no
 * darktable.develop state -- only the mutex-guarded capture snapshot. */
gboolean dt_remote_scopes_execute(const dt_remote_scopes_request_t *req,
                                  GCancellable *cancellable,
                                  dt_remote_scopes_result_t **out,
                                  dt_remote_error_t **error);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
