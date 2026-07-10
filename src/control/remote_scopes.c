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

void dt_remote_scopes_push(const float *hist_rgb, int width, int height,
                           const dt_iop_order_iccprofile_info_t *vs_prof)
{
  if(!hist_rgb || width <= 0 || height <= 0 || !vs_prof)
  {
    // clear the slot
    g_mutex_lock(&s_capture_mutex);
    _capture_t *old = s_capture;
    s_capture = NULL;
    g_mutex_unlock(&s_capture_mutex);
    _capture_free(old);
    return;
  }

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
  // pixelpipe-worker-thread context as the existing lib code.
  const dt_iop_order_iccprofile_info_t *hlg =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020,
                                      "", DT_INTENT_PERCEPTUAL);
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

  // Revision stamp: a thread-safe plain read of the process-local counter
  // (G_LOCK-guarded). The main-thread-only observe_image self-heal is NOT
  // available here, so this can read a slightly stale value if an image
  // change is mid-flight -- accepted: it fails toward a stale stamp plus a
  // retryable downstream conflict, never toward a fabricated value
  // (internals §9 capture design; flagged for controller ratification).
  c->revision = dt_remote_current_revision();

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
  g_mutex_unlock(&s_capture_mutex);
  _capture_free(old);
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
        DT_SCOPES_VEC_SCALE_LOGARITHMIC, diam, cap->gamma_lut, cap->gamma_lutsize);
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
