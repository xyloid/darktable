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

#include "control/remote_scopes.h"

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/iop_profile.h"
#include "control/control.h"    // dt_control_t (main-thread assert)
#include "control/signal.h"     // DT_SIGNAL_DEVELOP_IMAGE_CHANGED
#include "develop/develop.h"
#include "views/view.h"

#include <string.h>

// Local error constructor (dt_remote_error_new is file-static in
// remote_edit.c by design; each translation unit builds its own).
static dt_remote_error_t *_scopes_error_new(dt_remote_error_code_t code,
                                            const char *format, ...) G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *_scopes_error_new(dt_remote_error_code_t code,
                                            const char *format, ...)
{
  dt_remote_error_t *err = g_malloc0(sizeof(dt_remote_error_t));
  err->code = code;
  va_list args;
  va_start(args, format);
  err->message = g_strdup_vprintf(format, args);
  va_end(args);
  return err;
}

/* ---------------------------------------------------------------------- */
/* capture slot                                                            */
/* ---------------------------------------------------------------------- */

// Everything the background kernels need, deep-copied so the compute path
// never dereferences develop-owned memory off the main thread. The vs_prof
// matrices are inline; its lut_in is deep-copied only when the histogram
// profile is non-linear (a CLUT profile) -- for the common matrix profile
// (linear Rec2020) nonlinearlut==0 and the kernels never touch lut_in.
typedef struct _capture_t
{
  float *rgb;
  int width, height;
  dt_iop_order_iccprofile_info_t vs_prof;
  float *lut_in_copy[3];    // owned iff vs_prof.nonlinearlut
  float *gamma_lut;         // owned; HLG Rec2020 display-gamma LUT
  int gamma_lutsize;
  uint64_t revision;
} _capture_t;

static GMutex s_capture_mutex;
static _capture_t *s_capture = NULL;   // latest pushed buffer; NULL == empty slot

// The hot-path gate (finding 1). A single g_atomic int read on the preview
// pixelpipe worker thread decides, before any allocation or develop access,
// whether the remote server is active at all -- so darktable users without
// remote control pay nothing for the capture. Set TRUE by
// dt_remote_scopes_capture_start() (server start) and FALSE by
// dt_remote_scopes_capture_stop() (server stop), both on the main thread.
static gint s_capture_active = 0;

// Pipe-start/push revision equality proof (finding 2). Bumps to the
// process-local revision happen ONLY on the main thread (remote_revision.c),
// so if the revision read when the preview pipe fixed its input history
// equals the revision read at push time, no bump landed during the run and
// the captured pixels provably match their stamp. s_pipe_start_revision is
// written by dt_remote_scopes_note_pipe_start() and consumed once by the
// next push; both run in sequence on the same preview-pipe worker thread
// (serialized by the pipe mutex), and s_capture_mutex guards them against a
// concurrent full-pipe worker. s_pipe_start_valid makes the note one-shot:
// a push with no paired note (e.g. a tether-view push) has no coherence
// proof and is dropped.
static uint64_t s_pipe_start_revision = 0;
static gboolean s_pipe_start_valid = FALSE;

// Injectable revision getter (test seam). Production reads the live
// process-local counter; unit tests override it to drive the equal/unequal
// coherence branches deterministically without a develop context.
static uint64_t (*s_revision_getter)(void) = dt_remote_current_revision;

static uint64_t _current_revision(void)
{
  return s_revision_getter ? s_revision_getter() : 0;
}

static void _capture_free(_capture_t *c)
{
  if(!c) return;
  dt_free_align(c->rgb);
  for(int k = 0; k < 3; k++) g_free(c->lut_in_copy[k]);
  g_free(c->gamma_lut);
  g_free(c);
}

// Deep copy of the mutable profile bits the kernels read. Repoints lut_in
// at owned copies (only when non-linear); zeroes the out-direction luts the
// scope kernels never use so a stale develop pointer can never be followed.
static void _copy_profile(const dt_iop_order_iccprofile_info_t *src, _capture_t *dst)
{
  dst->vs_prof = *src;
  dst->vs_prof.lut_out[0] = dst->vs_prof.lut_out[1] = dst->vs_prof.lut_out[2] = NULL;
  for(int k = 0; k < 3; k++) dst->lut_in_copy[k] = NULL;
  if(src->nonlinearlut && src->lutsize > 0)
    for(int k = 0; k < 3; k++)
    {
      if(src->lut_in[k])
      {
        dst->lut_in_copy[k] = g_malloc((size_t)src->lutsize * sizeof(float));
        memcpy(dst->lut_in_copy[k], src->lut_in[k], (size_t)src->lutsize * sizeof(float));
        dst->vs_prof.lut_in[k] = dst->lut_in_copy[k];
      }
      else
        dst->vs_prof.lut_in[k] = NULL;
    }
}

void dt_remote_scopes_note_pipe_start(void)
{
  // Cheap gate first: nothing to prove when no server is capturing.
  if(!g_atomic_int_get(&s_capture_active)) return;
  const uint64_t rev = _current_revision();
  g_mutex_lock(&s_capture_mutex);
  s_pipe_start_revision = rev;
  s_pipe_start_valid = TRUE;
  g_mutex_unlock(&s_capture_mutex);
}

void dt_remote_scopes_push(const float *hist_rgb, int width, int height,
                           const dt_iop_order_iccprofile_info_t *vs_prof)
{
  // Finding 1: the single cheap atomic read that makes this a true no-op for
  // every darktable user without remote control -- BEFORE any allocation,
  // deep copy, or develop access.
  if(!g_atomic_int_get(&s_capture_active)) return;

  if(!hist_rgb || width <= 0 || height <= 0 || !vs_prof)
  {
    // clear the slot
    g_mutex_lock(&s_capture_mutex);
    _capture_t *old = s_capture;
    s_capture = NULL;
    s_pipe_start_valid = FALSE;
    g_mutex_unlock(&s_capture_mutex);
    _capture_free(old);
    return;
  }

  // Finding 2: prove stamp/pixel coherence before spending anything. Read the
  // revision now (push time) and compare against the pipe-start revision. An
  // exact match means no history bump landed on the main thread during the
  // run, so these pixels reflect exactly this revision. Consume the one-shot
  // note under the same lock. On any mismatch (or a push with no paired
  // note) DROP the push: leave the previous, coherent slot untouched. The
  // bump that caused the mismatch has already scheduled a fresh preview run
  // which will push a coherent buffer moments later; a compute_scopes in the
  // gap sees the older-but-coherent slot (acceptable) or an empty slot
  // (retryable scope_failed) -- never a mislabelled buffer.
  const uint64_t r_push = _current_revision();
  g_mutex_lock(&s_capture_mutex);
  const gboolean coherent = s_pipe_start_valid && s_pipe_start_revision == r_push;
  s_pipe_start_valid = FALSE;
  g_mutex_unlock(&s_capture_mutex);
  if(!coherent) return;

  _capture_t *c = g_malloc0(sizeof(_capture_t));
  c->width = width;
  c->height = height;

  const size_t n = (size_t)4 * width * height;
  c->rgb = dt_alloc_align_float(n);
  if(!c->rgb) { _capture_free(c); return; }
  memcpy(c->rgb, hist_rgb, n * sizeof(float));

  _copy_profile(vs_prof, c);

  // Resolve + copy the display-gamma (HLG Rec2020) LUT the waveform and
  // vectorscope kernels borrow, exactly as the GUI panel does. Same
  // pixelpipe-worker-thread context as the existing lib code. Guarded on a
  // live develop so the capture is safe to exercise without one (unit tests
  // and any pre-develop call fall through to the identity LUT below).
  const dt_iop_order_iccprofile_info_t *hlg = darktable.develop
    ? dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020,
                                        "", DT_INTENT_PERCEPTUAL)
    : NULL;
  if(hlg && hlg->lut_out[0] && hlg->lutsize > 0)
  {
    c->gamma_lutsize = hlg->lutsize;
    c->gamma_lut = g_malloc((size_t)hlg->lutsize * sizeof(float));
    memcpy(c->gamma_lut, hlg->lut_out[0], (size_t)hlg->lutsize * sizeof(float));
  }
  else
  {
    // Fallback identity LUT so a missing HLG profile can never leave the
    // kernels reading a NULL lut; degrades gamma, not correctness.
    c->gamma_lutsize = 256;
    c->gamma_lut = g_malloc(256 * sizeof(float));
    for(int i = 0; i < 256; i++) c->gamma_lut[i] = (float)i / 255.f;
  }

  // Revision stamp: r_push, already proven equal to the pipe-start revision
  // above, so it is exactly the revision these pixels reflect (finding 2).
  c->revision = r_push;

  g_mutex_lock(&s_capture_mutex);
  _capture_t *old = s_capture;
  s_capture = c;
  g_mutex_unlock(&s_capture_mutex);
  _capture_free(old);
}

void dt_remote_scopes_capture_reset(void)
{
  g_mutex_lock(&s_capture_mutex);
  _capture_t *old = s_capture;
  s_capture = NULL;
  s_pipe_start_valid = FALSE;
  g_mutex_unlock(&s_capture_mutex);
  _capture_free(old);
}

// Darkroom image switch (finding 2 / folded Minor F6): drop the slot so the
// previous image's buffer can never be served as another image's scopes.
// The next preview run for the new image will push a coherent buffer; a
// compute_scopes in the gap fails retryably. Runs on the main thread.
static void _on_image_changed(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;
  dt_remote_scopes_capture_reset();
}

void dt_remote_scopes_capture_start(void)
{
  // Clear any stale slot BEFORE arming the gate: after this a pre-connect
  // buffer cannot exist at all (folds Minor F5 -- previously the slot was
  // reset only at stop, so the first compute_scopes after connect could
  // serve a pre-connect buffer stamped revision 0).
  dt_remote_scopes_capture_reset();
  g_atomic_int_set(&s_capture_active, 1);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _on_image_changed, NULL);
}

void dt_remote_scopes_capture_stop(void)
{
  g_atomic_int_set(&s_capture_active, 0);
  DT_CONTROL_SIGNAL_DISCONNECT(_on_image_changed, NULL);
  dt_remote_scopes_capture_reset();
}

/* ---------------------------------------------------------------------- */
/* test seams                                                              */
/* ---------------------------------------------------------------------- */

void dt_remote_scopes_test_set_revision_getter(uint64_t (*getter)(void))
{
  s_revision_getter = getter ? getter : dt_remote_current_revision;
}

void dt_remote_scopes_test_set_active(gboolean active)
{
  g_atomic_int_set(&s_capture_active, active ? 1 : 0);
}

int64_t dt_remote_scopes_test_slot_revision(void)
{
  g_mutex_lock(&s_capture_mutex);
  const int64_t rev = s_capture ? (int64_t)s_capture->revision : -1;
  g_mutex_unlock(&s_capture_mutex);
  return rev;
}

// Deep-copies the current slot under the lock, so the compute path runs
// entirely off a private snapshot (the producer can push again freely).
static _capture_t *_capture_snapshot(void)
{
  g_mutex_lock(&s_capture_mutex);
  if(!s_capture)
  {
    g_mutex_unlock(&s_capture_mutex);
    return NULL;
  }
  const _capture_t *src = s_capture;
  _capture_t *c = g_malloc0(sizeof(_capture_t));
  c->width = src->width;
  c->height = src->height;
  c->revision = src->revision;
  const size_t n = (size_t)4 * src->width * src->height;
  c->rgb = dt_alloc_align_float(n);
  if(c->rgb) memcpy(c->rgb, src->rgb, n * sizeof(float));
  // profile: copy struct then repoint any owned luts
  c->vs_prof = src->vs_prof;
  for(int k = 0; k < 3; k++) c->lut_in_copy[k] = NULL;
  if(src->vs_prof.nonlinearlut && src->vs_prof.lutsize > 0)
    for(int k = 0; k < 3; k++)
      if(src->lut_in_copy[k])
      {
        c->lut_in_copy[k] = g_malloc((size_t)src->vs_prof.lutsize * sizeof(float));
        memcpy(c->lut_in_copy[k], src->lut_in_copy[k],
               (size_t)src->vs_prof.lutsize * sizeof(float));
        c->vs_prof.lut_in[k] = c->lut_in_copy[k];
      }
  c->gamma_lutsize = src->gamma_lutsize;
  c->gamma_lut = g_malloc((size_t)src->gamma_lutsize * sizeof(float));
  memcpy(c->gamma_lut, src->gamma_lut, (size_t)src->gamma_lutsize * sizeof(float));
  g_mutex_unlock(&s_capture_mutex);

  if(!c->rgb) { _capture_free(c); return NULL; }
  return c;
}

/* ---------------------------------------------------------------------- */
/* result lifecycle                                                        */
/* ---------------------------------------------------------------------- */

void dt_remote_scopes_result_free(dt_remote_scopes_result_t *result)
{
  if(!result) return;
  g_free(result->source);
  g_free(result->color_profile);
  g_free(result->roi);
  g_free(result->waveform.png);
  g_free(result->parade.png);
  g_free(result->vectorscope.png);
  g_free(result);
}

/* ---------------------------------------------------------------------- */
/* main-thread precondition                                                */
/* ---------------------------------------------------------------------- */

gboolean dt_remote_scopes_prepare(dt_remote_error_t **error)
{
  g_assert(!darktable.control || pthread_equal(darktable.control->gui_thread, pthread_self()));

  if(dt_view_get_current() != DT_VIEW_DARKROOM)
  {
    if(error)
      *error = _scopes_error_new(DT_REMOTE_ERR_NOT_IN_DARKROOM,
                                   _("no darkroom view is active"));
    return FALSE;
  }
  dt_develop_t *dev = darktable.develop;
  if(!dev || !dt_is_valid_imgid(dev->image_storage.id))
  {
    if(error)
      *error = _scopes_error_new(DT_REMOTE_ERR_NO_IMAGE_OPEN,
                                   _("no image is open in the darkroom"));
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* background compute                                                      */
/* ---------------------------------------------------------------------- */

// waveform/vectorscope resolution knobs (bounded so PNGs stay small).
#define DT_REMOTE_SCOPES_WAVE_TONES 256

static gboolean _encode_image(const uint8_t *rgb24, int w, int h, size_t stride,
                              dt_remote_scopes_image_t *out)
{
  GByteArray *png = dt_scopes_encode_png_rgb24(rgb24, w, h, stride);
  if(!png) return FALSE;
  out->present = TRUE;
  out->width = w;
  out->height = h;
  out->png_len = png->len;
  out->png = g_byte_array_free(png, FALSE);  // hand ownership to out->png
  return TRUE;
}

gboolean dt_remote_scopes_execute(const dt_remote_scopes_request_t *req,
                                  GCancellable *cancellable,
                                  dt_remote_scopes_result_t **out,
                                  dt_remote_error_t **error)
{
  if(!req || !out)
  {
    if(error) *error = _scopes_error_new(DT_REMOTE_ERR_INTERNAL, _("internal error: null argument"));
    return FALSE;
  }

  if(g_cancellable_is_cancelled(cancellable))
  {
    if(error) *error = _scopes_error_new(DT_REMOTE_ERR_SCOPE_FAILED, _("scope computation was cancelled"));
    return FALSE;
  }

  _capture_t *cap = _capture_snapshot();
  if(!cap)
  {
    // Empty slot (fresh darkroom, no preview run yet). scope_failed follows
    // the same fail-toward-retryable rule as preview_failed (ratified): pin
    // retryable=TRUE unconditionally -- another preview run will fill it.
    if(error)
      *error = _scopes_error_new(DT_REMOTE_ERR_SCOPE_FAILED,
                                   _("no preview buffer captured yet; retry after the preview updates"));
    return FALSE;
  }

  dt_remote_scopes_result_t *r = g_malloc0(sizeof(dt_remote_scopes_result_t));
  r->revision = cap->revision;
  r->source = g_strdup("final_preview");
  r->roi = g_strdup("full_image");
  r->color_profile = g_strdup(dt_colorspaces_get_name(cap->vs_prof.type, cap->vs_prof.filename));

  const dt_histogram_roi_t roi = { .width = cap->width, .height = cap->height,
                                   .crop_x = 0, .crop_y = 0, .crop_right = 0, .crop_bottom = 0 };

  const int image_size = req->image_size;   // already clamped [128,1024] by the handler

  gboolean ok = TRUE;

  if(req->want_histogram && (req->include_summary || req->include_bins))
  {
    r->has_histogram = TRUE;
    if(req->include_summary)
    {
      dt_scopes_histogram_summarize(cap->rgb, &roi, &r->histogram_summary);
      r->has_histogram_summary = TRUE;
    }
    if(req->include_bins)
    {
      dt_scopes_histogram_t hist;
      dt_scopes_histogram_compute(cap->rgb, &roi, &hist);
      const double denom = hist.max > 0 ? (double)hist.max : 1.0;
      for(int ch = 0; ch < 3; ch++)
        for(int b = 0; b < DT_SCOPES_HISTOGRAM_BINS; b++)
          r->histogram_bins[ch][b] = (float)(hist.bins[4 * b + ch] / denom);
      r->has_histogram_bins = TRUE;
    }
  }
  else if(req->want_histogram)
  {
    r->has_histogram = TRUE;   // requested but neither summary nor bins asked for
  }

  // waveform / parade share one compute (RGB parade is a presentation of the
  // same rasters) -- compute once when either is requested with images.
  if((req->want_waveform || req->want_parade) && req->include_images
     && !g_cancellable_is_cancelled(cancellable))
  {
    dt_scopes_waveform_t *wf = dt_scopes_waveform_alloc_compute(
        cap->rgb, &roi, DT_SCOPES_WAVE_ORIENT_HORI,
        image_size, DT_REMOTE_SCOPES_WAVE_TONES, cap->gamma_lut, cap->gamma_lutsize);
    if(wf)
    {
      if(req->want_waveform)
      {
        int w, h; size_t stride;
        uint8_t *rgb = dt_scopes_waveform_colorize(wf, FALSE, &w, &h, &stride);
        if(rgb) { ok = _encode_image(rgb, w, h, stride, &r->waveform) && ok; g_free(rgb); }
        else ok = FALSE;
      }
      if(req->want_parade)
      {
        int w, h; size_t stride;
        uint8_t *rgb = dt_scopes_waveform_colorize(wf, TRUE, &w, &h, &stride);
        if(rgb) { ok = _encode_image(rgb, w, h, stride, &r->parade) && ok; g_free(rgb); }
        else ok = FALSE;
      }
      dt_scopes_waveform_free(wf);
    }
    else ok = FALSE;
  }

  if(req->want_vectorscope && req->include_images
     && !g_cancellable_is_cancelled(cancellable))
  {
    const int diam = CLAMP(image_size, 128, 1024);
    dt_scopes_vectorscope_t *v = dt_scopes_vectorscope_alloc_compute(
        cap->rgb, &roi, &cap->vs_prof, DT_SCOPES_VEC_TYPE_CIELUV,
        DT_SCOPES_VS_SCALE_LOGARITHMIC, diam, cap->gamma_lut, cap->gamma_lutsize);
    if(v)
    {
      int dim; size_t stride;
      uint8_t *rgb = dt_scopes_vectorscope_colorize(v, &dim, &stride);
      if(rgb) { ok = _encode_image(rgb, dim, dim, stride, &r->vectorscope) && ok; g_free(rgb); }
      else ok = FALSE;
      dt_scopes_vectorscope_free(v);
    }
    else ok = FALSE;
  }

  _capture_free(cap);

  if(!ok)
  {
    dt_remote_scopes_result_free(r);
    if(error)
      *error = _scopes_error_new(DT_REMOTE_ERR_SCOPE_FAILED, _("scope image encoding failed"));
    return FALSE;
  }

  *out = r;
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
