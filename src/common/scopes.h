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

// Shared, statically-linkable scope kernels (histogram, waveform, RGB
// parade, vectorscope), factored out of the loadable `histogram` lib
// (src/libs/histogram.c + src/libs/scopes/*.c) so BOTH the GUI scopes
// panel and the MCP remote-edit `compute_scopes` service run one
// implementation over one captured final-preview buffer -- neither
// scrapes the other's private GtkWidget buffers (design spec, "Scope
// analysis"; internals §9).
//
// Everything here is a pure function over POD inputs/outputs (no
// GtkWidget, no cairo drawing state, no darktable.develop globals): the
// four compute kernels, the shared display->histogram-profile conversion
// the GUI and remote both use, cairo-free colorizers that composite the
// A8 rasters to RGB for export, and an in-memory PNG encoder. The lib's
// `*_process` functions become thin adapters that unpack their data
// structs into these POD types; the remote service runs the same kernels
// on a background job.

#pragma once

#include "common/histogram.h"      // dt_histogram_roi_t
#include "common/iop_profile.h"    // dt_iop_order_iccprofile_info_t

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---------------------------------------------------------------------- */
/* shared display -> histogram-profile conversion (internals §9.3)         */
/* ---------------------------------------------------------------------- */

/** Converts a final-preview buffer from display RGB to the histogram
 * profile, exactly as the GUI scopes panel does (src/libs/histogram.c:
 * 160-184): `input`/`out` are 4*width*height interleaved floats; `from`
 * is the display profile the pixelpipe emitted; `to` is the histogram
 * profile (may be NULL, in which case `fallback` -- normally linear
 * Rec2020 -- is used, matching the GUI's replacement behaviour). Returns
 * the profile actually used for the conversion, which downstream kernels
 * (vectorscope) take as their working `vs_prof`. Both `out` and the
 * returned pointer are borrowed; the caller owns `out`. */
const dt_iop_order_iccprofile_info_t *dt_scopes_convert_to_histogram_profile(
    const float *input, float *out, int width, int height,
    const dt_iop_order_iccprofile_info_t *from,
    const dt_iop_order_iccprofile_info_t *to,
    const dt_iop_order_iccprofile_info_t *fallback);

/* ---------------------------------------------------------------------- */
/* histogram kernel + numeric summary                                      */
/* ---------------------------------------------------------------------- */

#define DT_SCOPES_HISTOGRAM_BINS 256

// Per-channel bin counts, interleaved R,G,B,(pad) exactly like
// dt_histogram_helper's output (4 uint32 per bin), plus the max bin count
// across the three colour channels -- the same POD the lib's histogram
// data struct carries, minus the GtkWidget.
typedef struct dt_scopes_histogram_t
{
  uint32_t bins[4 * DT_SCOPES_HISTOGRAM_BINS];
  uint32_t max;   // MAX over the R, G, B channel maxima
} dt_scopes_histogram_t;

/** The histogram kernel: bins `input` (already in the histogram profile,
 * 4*roi->width*roi->height floats) into `out`, over the sampled ROI, with
 * 256 RGB bins -- delegating to the GUI-independent dt_histogram_helper so
 * the GUI adapter and the remote path produce identical numbers. */
void dt_scopes_histogram_compute(const float *input,
                                 const dt_histogram_roi_t *roi,
                                 dt_scopes_histogram_t *out);

// The model-oriented numeric summary the protocol returns (design spec
// "Scope analysis"): every field states its binning/normalization so the
// model can reason without interpreting a plot. Percentiles and clip
// fractions are luminance-based (Rec709 luma); channel means are the mean
// normalized [0,1] value per colour channel. Computed directly over the
// histogram-profile pixels so the luminance percentiles are exact rather
// than reconstructed from per-channel bins.
typedef struct dt_scopes_histogram_summary_t
{
  int bins;                          // always DT_SCOPES_HISTOGRAM_BINS
  double black_clip_fraction;        // fraction of pixels with luma <= 0
  double white_clip_fraction;        // fraction of pixels with luma >= 1
  double p01, p50, p99;              // luminance percentiles, [0,1]-ish
  double mean_red, mean_green, mean_blue;
} dt_scopes_histogram_summary_t;

/** Computes the numeric summary over the histogram-profile pixel buffer
 * and sampled ROI. Pure; independent of dt_scopes_histogram_compute. */
void dt_scopes_histogram_summarize(const float *input,
                                   const dt_histogram_roi_t *roi,
                                   dt_scopes_histogram_summary_t *out);

/** Renders the histogram bins to an RGB24 (BGRX byte order in a uint32,
 * i.e. cairo CAIRO_FORMAT_RGB24) bar graph of `width`x`height` for image
 * export -- the cairo-free equivalent of the lib's `_hist_draw`. Returns a
 * newly g_malloc'd buffer of `height * cairo_format_stride_for_width(
 * CAIRO_FORMAT_RGB24, width)` bytes (caller g_free), or NULL on failure;
 * `out_stride` receives the stride. `logarithmic` matches the GUI scale
 * toggle. */
uint8_t *dt_scopes_histogram_colorize(const dt_scopes_histogram_t *hist,
                                      int width, int height, gboolean logarithmic,
                                      size_t *out_stride);

/* ---------------------------------------------------------------------- */
/* waveform / parade kernel                                                */
/* ---------------------------------------------------------------------- */

typedef enum dt_scopes_wave_orient_t
{
  DT_SCOPES_WAVE_ORIENT_HORI = 0,
  DT_SCOPES_WAVE_ORIENT_VERT,
} dt_scopes_wave_orient_t;

// A8 rasters, one per RGB channel, plus their bin/tone geometry. For a
// horizontal waveform the raster is bins wide by tones high; for vertical
// it is tones wide by bins high. `img[ch]` are owned (dt_free_align) when
// produced by dt_scopes_waveform_alloc/compute into a fresh struct; the
// lib adapter instead points the kernel at its own preallocated buffers.
typedef struct dt_scopes_waveform_t
{
  uint8_t *img[3];
  int bins;                  // actual bins produced (varies with image)
  int tones;
  int max_bins;              // buffer capacity along the bin axis
  dt_scopes_wave_orient_t orient;
  gboolean owns_img;         // TRUE if img[] were allocated here
} dt_scopes_waveform_t;

/** Waveform/parade compute kernel (lifted from
 * src/libs/scopes/waveform.c:58-165 with `orient` a parameter): bins the
 * histogram-profile `input` over the ROI into the caller-provided A8
 * rasters `img[3]`, applying the display-gamma LUT `gamma_lut`
 * (`gamma_lutsize` entries -- the HLG Rec2020 lut_out the GUI borrows).
 * `max_bins`/`tones` size the output; `*out_bins` receives the actual bin
 * count. The rasters are written with cairo's A8 stride for the current
 * orientation (bins wide when horizontal, tones wide when vertical). Pure:
 * touches no globals. */
void dt_scopes_waveform_compute(const float *input,
                                const dt_histogram_roi_t *roi,
                                dt_scopes_wave_orient_t orient,
                                int max_bins, int tones,
                                const float *gamma_lut, int gamma_lutsize,
                                uint8_t *const img[3],
                                int *out_bins);

/** Allocates a waveform result sized for `max_bins`x`tones` (both
 * orientations fit), runs the kernel, and returns it (caller frees with
 * dt_scopes_waveform_free). NULL on allocation failure. */
dt_scopes_waveform_t *dt_scopes_waveform_alloc_compute(const float *input,
                                                       const dt_histogram_roi_t *roi,
                                                       dt_scopes_wave_orient_t orient,
                                                       int max_bins, int tones,
                                                       const float *gamma_lut,
                                                       int gamma_lutsize);

/** Frees a waveform result allocated by dt_scopes_waveform_alloc_compute
 * (only frees img[] when owns_img). NULL-safe. */
void dt_scopes_waveform_free(dt_scopes_waveform_t *w);

/** Composites the three A8 waveform rasters into an RGB24 image of the
 * raster's native dimensions (bins x tones or tones x bins depending on
 * orientation) -- a cairo-free additive colorizer: each channel's A8
 * coverage lights its own primary. `parade` lays the three channels
 * side-by-side (RGB parade) instead of overlaid. Returns a g_malloc'd
 * RGB24 buffer (caller g_free) and its dimensions/stride, or NULL. */
uint8_t *dt_scopes_waveform_colorize(const dt_scopes_waveform_t *w, gboolean parade,
                                     int *out_width, int *out_height, size_t *out_stride);

/* ---------------------------------------------------------------------- */
/* vectorscope kernel                                                      */
/* ---------------------------------------------------------------------- */

// NOTE: type/constant names deliberately differ from the lib's private
// dt_scopes_vec_vectorscope_type_t / dt_scopes_vec_scale_t enums so
// src/libs/scopes/vectorscope.c can include this header and adapt onto the
// shared kernel without redefinition collisions.
typedef enum dt_scopes_vec_type_t
{
  DT_SCOPES_VEC_TYPE_CIELUV = 0,   // CIE 1976 u*v*
  DT_SCOPES_VEC_TYPE_JZAZBZ,
  DT_SCOPES_VEC_TYPE_RYB,          // GUI presentation colorspace (needs a
                                   // rgb2ryb spline); protocol v1 never
                                   // requests it remotely
} dt_scopes_vec_type_t;

typedef enum dt_scopes_vs_scale_t
{
  DT_SCOPES_VS_SCALE_LOGARITHMIC = 0,
  DT_SCOPES_VS_SCALE_LINEAR,
} dt_scopes_vs_scale_t;

#define DT_SCOPES_VEC_HUES 48

// A8 chromaticity graph (diam x diam) plus the RGB24 hue-ring background
// and the hue-ring chromaticity points used to build it. `graph`/`bkgd`
// are owned (dt_free_align / g_free per owns_*). `hue_ring[k][i]` are the
// {u,v}-style chromaticity coordinates of the ring, `radius` its max
// extent -- everything the colorizer needs to lay out the plot.
typedef struct dt_scopes_vectorscope_t
{
  uint8_t *graph;        // A8, diam x diam (cairo A8 stride)
  uint8_t *bkgd;         // RGB24, diam x diam (cairo RGB24 stride), nullable
  int diameter;
  float radius;          // max chromaticity radius (post-scale)
  float hue_ring[6][DT_SCOPES_VEC_HUES][2];
  float hue_rgb[6][DT_SCOPES_VEC_HUES][3];   // display RGB along the ring
  dt_scopes_vec_type_t type;
  dt_scopes_vs_scale_t scale;
  gboolean owns_buffers;
} dt_scopes_vectorscope_t;

/** Computes the vectorscope hue ring (the max-chroma boundary traced along
 * the RGB-cube edges) into `out->hue_ring`/`out->hue_rgb` and sets
 * `out->radius` -- lifted from _lib_histogram_vectorscope_bkgd's math
 * (src/libs/scopes/vectorscope.c) minus all cairo. Pure over `vs_prof`,
 * `type`, `scale`. */
void dt_scopes_vectorscope_hue_ring(const dt_iop_order_iccprofile_info_t *vs_prof,
                                    dt_scopes_vec_type_t type,
                                    dt_scopes_vs_scale_t scale,
                                    dt_scopes_vectorscope_t *out);

/** The vectorscope binning kernel (lifted from vectorscope.c:494-684,
 * dropping the GUI-only colorpicker-overlay block): 2x2-averages the
 * histogram-profile `input` over the ROI, converts each sample to
 * chromaticity in `vs_prof`, log/linear-scales it, and accumulates into
 * the A8 `out->graph` with the display-gamma LUT. Requires
 * `out->radius` (and, for the remote path, `out->hue_ring`) from
 * dt_scopes_vectorscope_hue_ring() -- or, for the GUI adapter, from the
 * lib's own hue-ring/background pass. `rgb2ryb_ypp` is the cubic-spline
 * second-derivative table for the RYB hue transposition (interpolate_set
 * over dt_color_ryb_{x,y}_vtx); required iff `out->type` is RYB, NULL
 * otherwise. Pure: no darktable.develop, no colorpicker globals. */
void dt_scopes_vectorscope_compute(const float *input,
                                   const dt_histogram_roi_t *roi,
                                   const dt_iop_order_iccprofile_info_t *vs_prof,
                                   const float *gamma_lut, int gamma_lutsize,
                                   const float *rgb2ryb_ypp,
                                   dt_scopes_vectorscope_t *out);

/** Allocates a vectorscope result of `diameter` px, computes hue ring +
 * graph over `input`, and returns it (caller frees with
 * dt_scopes_vectorscope_free). NULL on failure. RYB is not supported
 * through this remote-path convenience wrapper (no hue-ring math for it);
 * type must be CIELUV or JZAZBZ. */
dt_scopes_vectorscope_t *dt_scopes_vectorscope_alloc_compute(
    const float *input, const dt_histogram_roi_t *roi,
    const dt_iop_order_iccprofile_info_t *vs_prof,
    dt_scopes_vec_type_t type, dt_scopes_vs_scale_t scale, int diameter,
    const float *gamma_lut, int gamma_lutsize);

/** Frees a vectorscope result. NULL-safe. */
void dt_scopes_vectorscope_free(dt_scopes_vectorscope_t *v);

/** Composites the A8 graph over a neutral hue-ring background into an
 * RGB24 image of `diameter`x`diameter` (caller g_free), or NULL. The
 * background is built cairo-side from the hue-ring chromaticities. */
uint8_t *dt_scopes_vectorscope_colorize(const dt_scopes_vectorscope_t *v,
                                        int *out_dim, size_t *out_stride);

/* ---------------------------------------------------------------------- */
/* PNG encoding (cairo in-memory; no file path exposed)                    */
/* ---------------------------------------------------------------------- */

/** PNG-encodes an RGB24 buffer (cairo CAIRO_FORMAT_RGB24 layout: each
 * pixel a native-endian uint32 0x00RRGGBB, rows padded to `stride`) of
 * `width`x`height` into a freshly allocated GByteArray via
 * cairo_surface_write_to_png_stream -- no temp file, no caller-selectable
 * path (design spec: in-memory destination only). Returns NULL on
 * failure. Caller g_byte_array_unref. */
GByteArray *dt_scopes_encode_png_rgb24(const uint8_t *rgb24, int width, int height,
                                       size_t stride);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
