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

#include "common/scopes.h"

#include "common/atomic.h"
#include "common/color_ryb.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/curve_tools.h"   // interpolate_val (RYB hue spline)
#include "common/darktable.h"
#include "common/math.h"

#include <cairo/cairo.h>
#include <float.h>
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* shared display -> histogram-profile conversion                          */
/* ---------------------------------------------------------------------- */

const dt_iop_order_iccprofile_info_t *dt_scopes_convert_to_histogram_profile(
    const float *input, float *out, int width, int height,
    const dt_iop_order_iccprofile_info_t *from,
    const dt_iop_order_iccprofile_info_t *to,
    const dt_iop_order_iccprofile_info_t *fallback)
{
  // Mirror src/libs/histogram.c:160-184 exactly so the GUI panel and the
  // remote service convert identically: a NULL histogram profile is
  // replaced by the (linear Rec2020) fallback.
  const dt_iop_order_iccprofile_info_t *profile_out = to ? to : fallback;
  dt_ioppr_transform_image_colorspace_rgb(input, out, width, height,
                                          from, profile_out, "scopes histogram");
  return profile_out;
}

/* ---------------------------------------------------------------------- */
/* histogram kernel                                                        */
/* ---------------------------------------------------------------------- */

void dt_scopes_histogram_compute(const float *input,
                                 const dt_histogram_roi_t *roi,
                                 dt_scopes_histogram_t *out)
{
  memset(out->bins, 0, sizeof(out->bins));
  out->max = 0;

  dt_dev_histogram_collection_params_t params = { 0 };
  params.roi = roi;
  params.bins_count = DT_SCOPES_HISTOGRAM_BINS;

  dt_dev_histogram_stats_t stats =
    { .bins_count = DT_SCOPES_HISTOGRAM_BINS,
      .ch = 4,
      .pixels = 0,
      .buf_size = sizeof(uint32_t) * 4 * DT_SCOPES_HISTOGRAM_BINS };

  uint32_t histogram_max[4] = { 0 };
  uint32_t *hist = out->bins;
  // Same call the GUI histogram mode makes (src/libs/scopes/histogram.c:
  // 80): RGB colourspace, 256 bins, no middle-grey compensation.
  dt_histogram_helper(&params, &stats, IOP_CS_RGB, IOP_CS_NONE,
                      input, &hist, histogram_max, FALSE, NULL);
  out->max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);
}

/* ---------------------------------------------------------------------- */
/* histogram numeric summary                                               */
/* ---------------------------------------------------------------------- */

void dt_scopes_histogram_summarize(const float *input,
                                   const dt_histogram_roi_t *roi,
                                   dt_scopes_histogram_summary_t *out)
{
  memset(out, 0, sizeof(*out));
  out->bins = DT_SCOPES_HISTOGRAM_BINS;

  const int x0 = roi->crop_x;
  const int y0 = roi->crop_y;
  const int x1 = roi->width - roi->crop_right;
  const int y1 = roi->height - roi->crop_bottom;
  const int w = roi->width;

  uint64_t luma_hist[DT_SCOPES_HISTOGRAM_BINS] = { 0 };
  uint64_t black = 0, white = 0, npix = 0;
  double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;

  for(int y = y0; y < y1; y++)
    for(int x = x0; x < x1; x++)
    {
      const float *px = input + 4U * ((size_t)y * w + x);
      const float r = px[0], g = px[1], b = px[2];
      sum_r += r; sum_g += g; sum_b += b;
      // Rec709 luma; clip fractions and percentiles are luminance-based
      // so the model gets one coherent tonal reading.
      const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      if(luma <= 0.0f) black++;
      if(luma >= 1.0f) white++;
      int bin = (int)(CLAMPS(luma, 0.0f, 1.0f) * (DT_SCOPES_HISTOGRAM_BINS - 1) + 0.5f);
      luma_hist[bin]++;
      npix++;
    }

  if(npix == 0) return;

  out->mean_red = sum_r / (double)npix;
  out->mean_green = sum_g / (double)npix;
  out->mean_blue = sum_b / (double)npix;
  out->black_clip_fraction = (double)black / (double)npix;
  out->white_clip_fraction = (double)white / (double)npix;

  // luminance percentiles from the cumulative luma histogram
  const double targets[3] = { 0.01, 0.50, 0.99 };
  double *dst[3] = { &out->p01, &out->p50, &out->p99 };
  int ti = 0;
  uint64_t cum = 0;
  for(int bin = 0; bin < DT_SCOPES_HISTOGRAM_BINS && ti < 3; bin++)
  {
    cum += luma_hist[bin];
    while(ti < 3 && (double)cum >= targets[ti] * (double)npix)
    {
      *dst[ti] = (double)bin / (double)(DT_SCOPES_HISTOGRAM_BINS - 1);
      ti++;
    }
  }
  while(ti < 3) { *dst[ti] = 1.0; ti++; }
}

/* ---------------------------------------------------------------------- */
/* histogram colorizer (cairo-free bins -> bars)                           */
/* ---------------------------------------------------------------------- */

// RGB24 pixel packing for cairo CAIRO_FORMAT_RGB24 (native-endian uint32
// 0x00RRGGBB).
static inline void _rgb24_set(uint8_t *row, int x, uint8_t r, uint8_t g, uint8_t b)
{
  uint32_t *p = (uint32_t *)row + x;
  *p = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

uint8_t *dt_scopes_histogram_colorize(const dt_scopes_histogram_t *hist,
                                      int width, int height, gboolean logarithmic,
                                      size_t *out_stride)
{
  if(width <= 0 || height <= 0) return NULL;
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  uint8_t *buf = g_malloc0(stride * height);
  if(!buf) return NULL;

  const double maxv = logarithmic ? log1p((double)hist->max) : (double)hist->max;
  if(maxv <= 0.0)
  {
    *out_stride = stride;
    return buf;   // no data: black frame
  }

  // For each output column, sample the corresponding histogram bin per
  // channel and paint an additive bar up to its normalized height. This
  // is the cairo-free analogue of _hist_draw's per-channel bars.
  for(int x = 0; x < width; x++)
  {
    const int bin = (int)((double)x / width * DT_SCOPES_HISTOGRAM_BINS);
    const int bin_c = MIN(DT_SCOPES_HISTOGRAM_BINS - 1, bin);
    int barh[3];
    for(int ch = 0; ch < 3; ch++)
    {
      const double v = (double)hist->bins[4 * bin_c + ch];
      const double sv = logarithmic ? log1p(v) : v;
      barh[ch] = (int)(sv / maxv * (height - 1));
    }
    for(int y = 0; y < height; y++)
    {
      const int from_bottom = height - 1 - y;
      uint8_t rgb[3] = { 0, 0, 0 };
      for(int ch = 0; ch < 3; ch++)
        if(from_bottom <= barh[ch]) rgb[ch] = 255;
      _rgb24_set(buf + (size_t)y * stride, x, rgb[0], rgb[1], rgb[2]);
    }
  }
  *out_stride = stride;
  return buf;
}

/* ---------------------------------------------------------------------- */
/* waveform / parade kernel                                                */
/* ---------------------------------------------------------------------- */

void dt_scopes_waveform_compute(const float *input,
                                const dt_histogram_roi_t *roi,
                                dt_scopes_wave_orient_t orient,
                                int max_bins, int tones,
                                const float *gamma_lut, int gamma_lutsize,
                                uint8_t *const img[3],
                                int *out_bins)
{
  // Lifted verbatim from src/libs/scopes/waveform.c:58-165, with `orient`,
  // `max_bins`, `tones` and the gamma LUT as parameters instead of the
  // lib data struct + darktable.develop.
  const int sample_width = MAX(1, roi->width - roi->crop_right - roi->crop_x);
  const int sample_height = MAX(1, roi->height - roi->crop_bottom - roi->crop_y);

  const int to_bin = orient == DT_SCOPES_WAVE_ORIENT_HORI ? sample_width : sample_height;
  const size_t samples_per_bin = ceilf(to_bin / (float)max_bins);
  const size_t num_bins = ceilf(to_bin / (float)samples_per_bin);
  *out_bins = (int)num_bins;
  const size_t num_tones = tones;

  const size_t wf_img_stride = cairo_format_stride_for_width(
      CAIRO_FORMAT_A8, orient == DT_SCOPES_WAVE_ORIENT_HORI ? num_bins : num_tones);

  for(int ch = 0; ch < 3; ch++)
  {
    const size_t rows = orient == DT_SCOPES_WAVE_ORIENT_HORI ? num_tones : num_bins;
    memset(img[ch], 0, wf_img_stride * rows);
  }

  size_t bin_pad;
  uint32_t *const restrict partial_binned =
    dt_calloc_perthread(3U * num_bins * num_tones, sizeof(uint32_t), &bin_pad);

  DT_OMP_FOR()
  for(size_t y = 0; y < (size_t)sample_height; y++)
  {
    const float *const restrict px = DT_IS_ALIGNED((const float *const restrict)input +
                                                   4U * ((y + roi->crop_y) * roi->width));
    uint32_t *const restrict binned = dt_get_perthread(partial_binned, bin_pad);
    for(size_t x = 0; x < (size_t)sample_width; x++)
    {
      const size_t bin = (orient == DT_SCOPES_WAVE_ORIENT_HORI ? x : y) / samples_per_bin;
      size_t tone[4] DT_ALIGNED_PIXEL;
      for_each_channel(ch, aligned(px, tone:16))
      {
        const float v = (8.0f / 9.0f) * px[4U * (x + roi->crop_x) + ch];
        tone[ch] = ceilf(CLAMPS(v, 0.0f, 1.0f) * (num_tones - 1));
      }
      for(size_t ch = 0; ch < 3; ch++)
        binned[num_tones * (ch * num_bins + bin) + tone[ch]]++;
    }
  }

  const float *const restrict lut = gamma_lut;
  const float lutmax = gamma_lutsize - 1;
  const float brightness = num_tones / 40.0f;
  const float scale = brightness / ((orient == DT_SCOPES_WAVE_ORIENT_HORI
                                     ? sample_height
                                     : sample_width) * samples_per_bin);
  const size_t nthreads = dt_get_num_threads();

  DT_OMP_FOR(collapse(3))
  for(size_t ch = 0; ch < 3; ch++)
    for(size_t bin = 0; bin < num_bins; bin++)
      for(size_t tone = 0; tone < num_tones; tone++)
      {
        uint8_t *const restrict wf_img = DT_IS_ALIGNED((uint8_t *const restrict)img[ch]);
        uint32_t acc = 0;
        for(size_t n = 0; n < nthreads; n++)
        {
          uint32_t *const restrict binned = dt_get_bythread(partial_binned, bin_pad, n);
          acc += binned[num_tones * (ch * num_bins + bin) + tone];
        }
        const float linear = MIN(1.f, scale * acc);
        const uint8_t display = lut[(int)(linear * lutmax)] * 255.f;
        if(orient == DT_SCOPES_WAVE_ORIENT_HORI)
          wf_img[tone * wf_img_stride + bin] = display;
        else
          wf_img[bin * wf_img_stride + tone] = display;
      }

  dt_free_align(partial_binned);
}

dt_scopes_waveform_t *dt_scopes_waveform_alloc_compute(const float *input,
                                                       const dt_histogram_roi_t *roi,
                                                       dt_scopes_wave_orient_t orient,
                                                       int max_bins, int tones,
                                                       const float *gamma_lut,
                                                       int gamma_lutsize)
{
  dt_scopes_waveform_t *w = g_malloc0(sizeof(dt_scopes_waveform_t));
  w->orient = orient;
  w->tones = tones;
  w->max_bins = max_bins;
  w->owns_img = TRUE;

  // buffer must fit both orientations, as the lib does
  const size_t bytes_hori = tones * cairo_format_stride_for_width(CAIRO_FORMAT_A8, max_bins);
  const size_t bytes_vert = max_bins * cairo_format_stride_for_width(CAIRO_FORMAT_A8, tones);
  const size_t bytes = MAX(bytes_hori, bytes_vert);
  for(int ch = 0; ch < 3; ch++)
  {
    w->img[ch] = dt_alloc_align_uint8(bytes);
    if(!w->img[ch]) { dt_scopes_waveform_free(w); return NULL; }
  }

  dt_scopes_waveform_compute(input, roi, orient, max_bins, tones, gamma_lut, gamma_lutsize,
                             w->img, &w->bins);
  return w;
}

void dt_scopes_waveform_free(dt_scopes_waveform_t *w)
{
  if(!w) return;
  if(w->owns_img)
    for(int ch = 0; ch < 3; ch++) dt_free_align(w->img[ch]);
  g_free(w);
}

uint8_t *dt_scopes_waveform_colorize(const dt_scopes_waveform_t *w, gboolean parade,
                                     int *out_width, int *out_height, size_t *out_stride)
{
  if(w->bins <= 0) return NULL;

  // native raster dimensions
  const int rw = w->orient == DT_SCOPES_WAVE_ORIENT_HORI ? w->bins : w->tones;
  const int rh = w->orient == DT_SCOPES_WAVE_ORIENT_HORI ? w->tones : w->bins;
  const size_t a8_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, rw);

  const int width = parade ? rw * 3 : rw;
  const int height = rh;
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  uint8_t *buf = g_malloc0(stride * height);
  if(!buf) return NULL;

  for(int y = 0; y < rh; y++)
  {
    uint8_t *orow = buf + (size_t)y * stride;
    for(int x = 0; x < rw; x++)
    {
      uint8_t cov[3];
      for(int ch = 0; ch < 3; ch++)
        cov[ch] = w->img[ch][(size_t)y * a8_stride + x];
      if(parade)
      {
        // three side-by-side panels, each its own primary
        for(int ch = 0; ch < 3; ch++)
        {
          uint8_t rgb[3] = { 0, 0, 0 };
          rgb[ch] = cov[ch];
          _rgb24_set(orow, ch * rw + x, rgb[0], rgb[1], rgb[2]);
        }
      }
      else
      {
        // additive overlay: each channel lights its primary
        _rgb24_set(orow, x, cov[0], cov[1], cov[2]);
      }
    }
  }

  *out_width = width;
  *out_height = height;
  *out_stride = stride;
  return buf;
}

/* ---------------------------------------------------------------------- */
/* vectorscope kernel                                                      */
/* ---------------------------------------------------------------------- */

#define VECTORSCOPE_BASE_LOG 30

static inline float _vec_baselog(float x, const float bound)
{
  return log1pf((VECTORSCOPE_BASE_LOG - 1.f) * x / bound) / logf(VECTORSCOPE_BASE_LOG) * bound;
}

static inline void _vec_log_scale(float *x, float *y, const float r)
{
  const float h = dt_fast_hypotf(*x, *y);
  if(h >= FLT_MIN)
  {
    const float s = _vec_baselog(h, r) / h;
    *x *= s;
    *y *= s;
  }
}

// RGB -> RYB hue transposition via the Gossett cube-hue spline (lifted
// verbatim from the lib's _rgb2ryb; dt_color_ryb_{x,y}_vtx come from
// common/color_ryb.h, `rgb2ryb_ypp` is the caller's interpolate_set table).
static void _vec_rgb2ryb(const dt_aligned_pixel_t rgb,
                         dt_aligned_pixel_t ryb,
                         const float *rgb2ryb_ypp)
{
  dt_aligned_pixel_t HSV;
  dt_RGB_2_HSV(rgb, HSV);
  HSV[0] = interpolate_val(sizeof(dt_color_ryb_x_vtx)/sizeof(float), (float *)dt_color_ryb_x_vtx, HSV[0],
                           (float *)dt_color_ryb_y_vtx, (float *)rgb2ryb_ypp, CUBIC_SPLINE);
  dt_HSV_2_RGB(HSV, ryb);
}

static void _vec_chromaticity(const dt_aligned_pixel_t RGB,
                              dt_aligned_pixel_t chromaticity,
                              const dt_scopes_vec_type_t vs_type,
                              const dt_iop_order_iccprofile_info_t *vs_prof,
                              const float *rgb2ryb_ypp)
{
  // Lifted from _get_chromaticity (vectorscope.c:419-492): CIELUV, JzAzBz
  // and RYB branches (RYB is GUI-only but must stay in the shared kernel
  // so the lib's process can be a thin adapter over it).
  for(int ch = 0; ch < 4; ch++) chromaticity[ch] = 0.f;
  switch(vs_type)
  {
    case DT_SCOPES_VEC_TYPE_CIELUV:
    {
      dt_aligned_pixel_t XYZ_D50;
      dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, vs_prof->matrix_in_transposed, vs_prof->lut_in,
                                 vs_prof->unbounded_coeffs_in, vs_prof->lutsize,
                                 vs_prof->nonlinearlut);
      dt_aligned_pixel_t xyY_D50;
      dt_D50_XYZ_to_xyY(XYZ_D50, xyY_D50);
      dt_xyY_to_Luv(xyY_D50, chromaticity);
      break;
    }
    case DT_SCOPES_VEC_TYPE_JZAZBZ:
    {
      dt_aligned_pixel_t XYZ_D50;
      dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, vs_prof->matrix_in_transposed, vs_prof->lut_in,
                                 vs_prof->unbounded_coeffs_in, vs_prof->lutsize,
                                 vs_prof->nonlinearlut);
      dt_aligned_pixel_t XYZ_D65;
      dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
      dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
      break;
    }
    case DT_SCOPES_VEC_TYPE_RYB:
    {
      dt_aligned_pixel_t RYB, rgb, HCV;
      dt_sRGB_to_linear_sRGB(RGB, rgb);
      _vec_rgb2ryb(rgb, RYB, rgb2ryb_ypp);
      dt_RGB_2_HCV(RYB, HCV);
      const float alpha = DT_2PI_F * HCV[0];
      chromaticity[1] = cosf(alpha) * HCV[1] * 0.01;
      chromaticity[2] = sinf(alpha) * HCV[1] * 0.01;
      break;
    }
  }
}

void dt_scopes_vectorscope_hue_ring(const dt_iop_order_iccprofile_info_t *vs_prof,
                                    dt_scopes_vec_type_t type,
                                    dt_scopes_vs_scale_t scale,
                                    dt_scopes_vectorscope_t *out)
{
  out->type = type;
  out->scale = scale;

  const float vertex_rgb[6][4] DT_ALIGNED_PIXEL = {
      { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f },
      { 0.f, 1.f, 1.f }, { 0.f, 0.f, 1.f }, { 1.f, 0.f, 1.f } };

  float max_radius = 0.f;
  for(int k = 0; k < 6; k++)
  {
    dt_aligned_pixel_t delta;
    for_each_channel(ch, aligned(vertex_rgb, delta:16))
      delta[ch] = (vertex_rgb[(k + 1) % 6][ch] - vertex_rgb[k][ch]) / DT_SCOPES_VEC_HUES;
    for(int i = 0; i < DT_SCOPES_VEC_HUES; i++)
    {
      dt_aligned_pixel_t rgb_scope, XYZ_D50 = { 0 }, chromaticity = { 0 }, rgb_display = { 0 };
      for_each_channel(ch, aligned(vertex_rgb, delta, rgb_scope:16))
        rgb_scope[ch] = vertex_rgb[k][ch] + delta[ch] * i;

      dt_ioppr_rgb_matrix_to_xyz(rgb_scope, XYZ_D50, vs_prof->matrix_in_transposed,
                                 vs_prof->lut_in, vs_prof->unbounded_coeffs_in,
                                 vs_prof->lutsize, vs_prof->nonlinearlut);
      if(type == DT_SCOPES_VEC_TYPE_CIELUV)
      {
        dt_aligned_pixel_t xyY;
        dt_D50_XYZ_to_xyY(XYZ_D50, xyY);
        dt_xyY_to_Luv(xyY, chromaticity);
      }
      else
      {
        dt_aligned_pixel_t XYZ_D65;
        dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
        dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
      }
      dt_XYZ_to_Rec709_D50(XYZ_D50, rgb_display);

      out->hue_ring[k][i][0] = chromaticity[1];
      out->hue_ring[k][i][1] = chromaticity[2];
      const float h = dt_fast_hypotf(chromaticity[1], chromaticity[2]);
      max_radius = MAX(max_radius, h);

      const float max_RGB = MAX(MAX(rgb_display[0], rgb_display[1]), rgb_display[2]);
      for(int ch = 0; ch < 3; ch++)
        out->hue_rgb[k][i][ch] = max_RGB > 0.f ? CLAMPS(rgb_display[ch] / max_RGB, 0.f, 1.f) : 0.f;
    }
  }

  if(scale == DT_SCOPES_VS_SCALE_LOGARITHMIC)
    for(int k = 0; k < 6; k++)
      for(int i = 0; i < DT_SCOPES_VEC_HUES; i++)
        _vec_log_scale(&out->hue_ring[k][i][0], &out->hue_ring[k][i][1], max_radius);

  out->radius = max_radius;
}

void dt_scopes_vectorscope_compute(const float *input,
                                   const dt_histogram_roi_t *roi,
                                   const dt_iop_order_iccprofile_info_t *vs_prof,
                                   const float *gamma_lut, int gamma_lutsize,
                                   const float *rgb2ryb_ypp,
                                   dt_scopes_vectorscope_t *out)
{
  const int diam_px = out->diameter;
  const dt_scopes_vec_type_t vs_type = out->type;
  const dt_scopes_vs_scale_t vs_scale = out->scale;
  const float max_radius = out->radius;
  const float max_diam = max_radius * 2.f;

  int sample_width = MAX(1, roi->width - roi->crop_right - roi->crop_x);
  int sample_height = MAX(1, roi->height - roi->crop_bottom - roi->crop_y);

  dt_atomic_int *const restrict binned = (dt_atomic_int *)dt_calloc_align_int(diam_px * diam_px);

  const int sample_max_x = sample_width - (sample_width % 2);
  const int sample_max_y = sample_height - (sample_height % 2);

  DT_OMP_FOR(collapse(2))
  for(size_t y = 0; y < (size_t)sample_max_y; y += 2)
    for(size_t x = 0; x < (size_t)sample_max_x; x += 2)
    {
      dt_aligned_pixel_t RGB = { 0.f }, chromaticity;
      const float *const restrict px =
        DT_IS_ALIGNED((const float *const restrict)input +
                      4U * ((y + roi->crop_y) * roi->width + x + roi->crop_x));
      for(size_t xx = 0; xx < 2; xx++)
        for(size_t yy = 0; yy < 2; yy++)
          for_each_channel(ch, aligned(px, RGB:16))
            RGB[ch] += px[4U * (yy * roi->width + xx) + ch] * 0.25f;

      _vec_chromaticity(RGB, chromaticity, vs_type, vs_prof, rgb2ryb_ypp);
      if(vs_scale == DT_SCOPES_VS_SCALE_LOGARITHMIC)
        _vec_log_scale(&chromaticity[1], &chromaticity[2], max_radius);

      const int out_x = (diam_px - 1) * (chromaticity[1] / max_diam + 0.5f);
      const int out_y = (diam_px - 1) * (chromaticity[2] / max_diam + 0.5f);
      if(out_x >= 0 && out_x <= diam_px - 1 && out_y >= 0 && out_y <= diam_px - 1)
        dt_atomic_add_int(binned + out_y * diam_px + out_x, 1);
    }

  const float *const restrict lut = gamma_lut;
  const float lutmax = gamma_lutsize - 1;
  const int out_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam_px);
  uint8_t *const graph = out->graph;
  memset(graph, 0, (size_t)out_stride * diam_px);

  const float gain = 1.f / 30.f;
  const float scale = gain * (diam_px * diam_px) / (float)(sample_width * sample_height);

  for(int out_y = 0; out_y < diam_px; out_y++)
    for(int out_x = 0; out_x < diam_px; out_x++)
    {
      const int count = binned[out_y * diam_px + out_x];
      const float intensity = lut[(int)(MIN(1.f, scale * count) * lutmax)];
      graph[out_y * out_stride + out_x] = intensity * 255.0f;
    }

  dt_free_align((void *)binned);
}

dt_scopes_vectorscope_t *dt_scopes_vectorscope_alloc_compute(
    const float *input, const dt_histogram_roi_t *roi,
    const dt_iop_order_iccprofile_info_t *vs_prof,
    dt_scopes_vec_type_t type, dt_scopes_vs_scale_t scale, int diameter,
    const float *gamma_lut, int gamma_lutsize)
{
  dt_scopes_vectorscope_t *v = g_malloc0(sizeof(dt_scopes_vectorscope_t));
  v->diameter = diameter;
  v->owns_buffers = TRUE;
  const size_t a8_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, diameter);
  v->graph = dt_alloc_align_uint8(a8_stride * diameter);
  if(!v->graph) { dt_scopes_vectorscope_free(v); return NULL; }

  dt_scopes_vectorscope_hue_ring(vs_prof, type, scale, v);
  dt_scopes_vectorscope_compute(input, roi, vs_prof, gamma_lut, gamma_lutsize,
                                NULL /* RYB not supported on this path */, v);
  return v;
}

void dt_scopes_vectorscope_free(dt_scopes_vectorscope_t *v)
{
  if(!v) return;
  if(v->owns_buffers)
  {
    dt_free_align(v->graph);
    g_free(v->bkgd);
  }
  g_free(v);
}

uint8_t *dt_scopes_vectorscope_colorize(const dt_scopes_vectorscope_t *v,
                                        int *out_dim, size_t *out_stride)
{
  const int diam = v->diameter;
  if(diam <= 0 || v->radius <= 0.f) return NULL;
  const size_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, diam);
  const size_t a8_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam);
  uint8_t *buf = g_malloc0(stride * diam);
  if(!buf) return NULL;

  // Neutral dark background with the A8 graph painted in a warm white --
  // the model reads chromaticity density; the hue-ring wheel colours are a
  // GUI nicety we approximate with a faint ring tint rather than a cairo
  // mesh gradient (kept cairo-free per internals §9.4).
  for(int y = 0; y < diam; y++)
  {
    uint8_t *orow = buf + (size_t)y * stride;
    for(int x = 0; x < diam; x++)
    {
      const uint8_t g = v->graph[(size_t)y * a8_stride + x];
      _rgb24_set(orow, x, g, g, g);
    }
  }
  *out_dim = diam;
  *out_stride = stride;
  return buf;
}

/* ---------------------------------------------------------------------- */
/* PNG encoding (cairo in-memory)                                          */
/* ---------------------------------------------------------------------- */

static cairo_status_t _png_write_cb(void *closure, const unsigned char *data, unsigned int length)
{
  g_byte_array_append((GByteArray *)closure, data, length);
  return CAIRO_STATUS_SUCCESS;
}

GByteArray *dt_scopes_encode_png_rgb24(const uint8_t *rgb24, int width, int height,
                                       size_t stride)
{
  if(!rgb24 || width <= 0 || height <= 0) return NULL;

  cairo_surface_t *surface = cairo_image_surface_create_for_data(
      (unsigned char *)rgb24, CAIRO_FORMAT_RGB24, width, height, (int)stride);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    return NULL;
  }

  GByteArray *out = g_byte_array_new();
  const cairo_status_t st = cairo_surface_write_to_png_stream(surface, _png_write_cb, out);
  cairo_surface_destroy(surface);
  if(st != CAIRO_STATUS_SUCCESS)
  {
    g_byte_array_unref(out);
    return NULL;
  }
  return out;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
