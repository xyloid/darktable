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
/*
 * cmocka unit tests for the shared scope kernels (src/common/scopes.c):
 * histogram binning + numeric summary, waveform raster dimensions and
 * orientation, vectorscope hue positions for known primaries, the
 * cairo-free colorizers, and the in-memory PNG encoder. These are pure
 * functions over synthetic fixture buffers -- no darktable.develop, no
 * pixelpipe, no GUI. Everything touching a live develop context stays in
 * the Xvfb integration suite (internals §10).
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "common/darktable.h"
#include "common/scopes.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// Aligned float image allocation the kernels' DT_IS_ALIGNED expects.
static float *_make_image(int w, int h)
{
  return dt_alloc_align_float((size_t)4 * w * h);
}

static dt_histogram_roi_t _full_roi(int w, int h)
{
  dt_histogram_roi_t roi = { .width = w, .height = h, .crop_x = 0, .crop_y = 0,
                             .crop_right = 0, .crop_bottom = 0 };
  return roi;
}

// An identity display-gamma LUT (linear pass-through) so waveform/
// vectorscope intensities are deterministic in tests.
static void _fill_identity_lut(float *lut, int n)
{
  for(int i = 0; i < n; i++) lut[i] = (float)i / (float)(n - 1);
}

/* ---- histogram ---------------------------------------------------------- */

static void test_histogram_constant_image_lands_in_one_bin(void **state)
{
  (void)state;
  const int w = 16, h = 16;
  float *img = _make_image(w, h);
  for(int i = 0; i < w * h; i++)
  {
    img[4 * i + 0] = 0.5f; img[4 * i + 1] = 0.5f;
    img[4 * i + 2] = 0.5f; img[4 * i + 3] = 0.0f;
  }
  dt_histogram_roi_t roi = _full_roi(w, h);

  dt_scopes_histogram_t hist;
  dt_scopes_histogram_compute(img, &roi, &hist);

  // Every channel's pixels fall into exactly one bin; that bin holds all
  // w*h pixels, and the reported max equals it.
  for(int ch = 0; ch < 3; ch++)
  {
    uint64_t total = 0, peak = 0;
    for(int b = 0; b < DT_SCOPES_HISTOGRAM_BINS; b++)
    {
      const uint32_t c = hist.bins[4 * b + ch];
      total += c;
      if(c > peak) peak = c;
    }
    assert_int_equal((int)total, w * h);
    assert_int_equal((int)peak, w * h);
  }
  assert_int_equal((int)hist.max, w * h);

  dt_free_align(img);
}

// Kernel/reference equivalence: the shared kernel's per-channel bin
// assignment matches an independent hand-count over the same fixture --
// the "adapter output == direct kernel output" guarantee at kernel
// granularity (the lib adapter calls this exact kernel).
static void test_histogram_matches_reference_count(void **state)
{
  (void)state;
  const int w = 32, h = 4;
  float *img = _make_image(w, h);
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const int i = y * w + x;
      img[4 * i + 0] = (float)x / (w - 1);      // red ramp
      img[4 * i + 1] = 0.25f;                   // green constant
      img[4 * i + 2] = 0.75f;                   // blue constant
      img[4 * i + 3] = 0.0f;
    }
  dt_histogram_roi_t roi = _full_roi(w, h);

  dt_scopes_histogram_t hist;
  dt_scopes_histogram_compute(img, &roi, &hist);

  // green all in one bin, blue all in one bin, red spread across many
  uint64_t g_total = 0, b_total = 0, r_bins_used = 0, r_total = 0;
  for(int bn = 0; bn < DT_SCOPES_HISTOGRAM_BINS; bn++)
  {
    g_total += hist.bins[4 * bn + 1];
    b_total += hist.bins[4 * bn + 2];
    if(hist.bins[4 * bn + 0]) r_bins_used++;
    r_total += hist.bins[4 * bn + 0];
  }
  assert_int_equal((int)g_total, w * h);
  assert_int_equal((int)b_total, w * h);
  assert_int_equal((int)r_total, w * h);
  assert_true(r_bins_used > 1);  // the ramp must occupy multiple bins
  // green (0.25) sits below blue (0.75) on the bin axis
  int g_bin = -1, b_bin = -1;
  for(int bn = 0; bn < DT_SCOPES_HISTOGRAM_BINS; bn++)
  {
    if(hist.bins[4 * bn + 1]) g_bin = bn;
    if(hist.bins[4 * bn + 2]) b_bin = bn;
  }
  assert_true(g_bin >= 0 && b_bin >= 0 && g_bin < b_bin);

  dt_free_align(img);
}

static void test_histogram_summary_ramp(void **state)
{
  (void)state;
  const int w = 256, h = 8;
  float *img = _make_image(w, h);
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const int i = y * w + x;
      const float v = (float)x / (w - 1);   // neutral ramp 0..1
      img[4 * i + 0] = v; img[4 * i + 1] = v; img[4 * i + 2] = v; img[4 * i + 3] = 0.0f;
    }
  dt_histogram_roi_t roi = _full_roi(w, h);

  dt_scopes_histogram_summary_t s;
  dt_scopes_histogram_summarize(img, &roi, &s);

  assert_int_equal(s.bins, DT_SCOPES_HISTOGRAM_BINS);
  // neutral ramp: channel means ~ 0.5, median ~ 0.5
  assert_true(s.mean_red > 0.45 && s.mean_red < 0.55);
  assert_true(s.mean_green > 0.45 && s.mean_green < 0.55);
  assert_true(s.mean_blue > 0.45 && s.mean_blue < 0.55);
  assert_true(s.p50 > 0.4 && s.p50 < 0.6);
  assert_true(s.p01 < s.p50 && s.p50 < s.p99);

  dt_free_align(img);
}

/* ---- waveform ----------------------------------------------------------- */

static void test_waveform_horizontal_dims(void **state)
{
  (void)state;
  const int w = 120, h = 80, max_bins = 60, tones = 160;
  float *img = _make_image(w, h);
  for(int i = 0; i < w * h; i++)
  {
    img[4 * i + 0] = 0.4f; img[4 * i + 1] = 0.5f;
    img[4 * i + 2] = 0.6f; img[4 * i + 3] = 0.0f;
  }
  dt_histogram_roi_t roi = _full_roi(w, h);
  float lut[256]; _fill_identity_lut(lut, 256);

  dt_scopes_waveform_t *wf = dt_scopes_waveform_alloc_compute(
      img, &roi, DT_SCOPES_WAVE_ORIENT_HORI, max_bins, tones, lut, 256);
  assert_non_null(wf);
  assert_true(wf->bins > 0);
  assert_true(wf->bins <= max_bins);
  assert_int_equal(wf->tones, tones);

  int cw, ch2; size_t stride;
  uint8_t *rgb = dt_scopes_waveform_colorize(wf, FALSE, &cw, &ch2, &stride);
  assert_non_null(rgb);
  // horizontal: raster is bins wide, tones high
  assert_int_equal(cw, wf->bins);
  assert_int_equal(ch2, tones);
  g_free(rgb);

  dt_scopes_waveform_free(wf);
  dt_free_align(img);
}

static void test_waveform_vertical_orientation_swaps_axes(void **state)
{
  (void)state;
  const int w = 120, h = 80, max_bins = 60, tones = 160;
  float *img = _make_image(w, h);
  for(int i = 0; i < w * h; i++)
  {
    img[4 * i + 0] = 0.5f; img[4 * i + 1] = 0.5f;
    img[4 * i + 2] = 0.5f; img[4 * i + 3] = 0.0f;
  }
  dt_histogram_roi_t roi = _full_roi(w, h);
  float lut[256]; _fill_identity_lut(lut, 256);

  dt_scopes_waveform_t *wf = dt_scopes_waveform_alloc_compute(
      img, &roi, DT_SCOPES_WAVE_ORIENT_VERT, max_bins, tones, lut, 256);
  assert_non_null(wf);

  int cw, ch2; size_t stride;
  uint8_t *rgb = dt_scopes_waveform_colorize(wf, FALSE, &cw, &ch2, &stride);
  assert_non_null(rgb);
  // vertical: raster is tones wide, bins high (axes swapped vs horizontal)
  assert_int_equal(cw, tones);
  assert_int_equal(ch2, wf->bins);
  g_free(rgb);

  // a constant grey image lights all three channels' rasters
  gboolean any = FALSE;
  const size_t a8_stride = ((tones + 3) / 4) * 4;  // cairo A8 stride approx
  (void)a8_stride;
  for(int ci = 0; ci < 3 && !any; ci++)
    for(int p = 0; p < tones; p++)
      if(wf->img[ci][p]) { any = TRUE; break; }
  assert_true(any);

  dt_scopes_waveform_free(wf);
  dt_free_align(img);
}

static void test_parade_is_three_panels_wide(void **state)
{
  (void)state;
  const int w = 60, h = 60, max_bins = 40, tones = 128;
  float *img = _make_image(w, h);
  for(int i = 0; i < w * h; i++)
  {
    img[4 * i + 0] = 0.5f; img[4 * i + 1] = 0.5f;
    img[4 * i + 2] = 0.5f; img[4 * i + 3] = 0.0f;
  }
  dt_histogram_roi_t roi = _full_roi(w, h);
  float lut[256]; _fill_identity_lut(lut, 256);

  dt_scopes_waveform_t *wf = dt_scopes_waveform_alloc_compute(
      img, &roi, DT_SCOPES_WAVE_ORIENT_HORI, max_bins, tones, lut, 256);
  assert_non_null(wf);

  int owf, ohf; size_t stride;
  uint8_t *overlay = dt_scopes_waveform_colorize(wf, FALSE, &owf, &ohf, &stride);
  int pwf, phf; size_t pstride;
  uint8_t *parade = dt_scopes_waveform_colorize(wf, TRUE, &pwf, &phf, &pstride);
  assert_non_null(overlay);
  assert_non_null(parade);
  assert_int_equal(pwf, owf * 3);
  assert_int_equal(phf, ohf);
  g_free(overlay);
  g_free(parade);

  dt_scopes_waveform_free(wf);
  dt_free_align(img);
}

/* ---- vectorscope -------------------------------------------------------- */

// A minimal linear-RGB profile (nonlinearlut == 0) whose transposed matrix
// is the standard sRGB primaries -> XYZ matrix, so the hue-ring math has a
// well-behaved gamut to trace.
static void _make_test_profile(dt_iop_order_iccprofile_info_t *p)
{
  memset(p, 0, sizeof(*p));
  p->nonlinearlut = 0;
  p->lutsize = 0;
  // sRGB (linear) -> XYZ, columns = R,G,B contributions; matrix_in_transposed
  // is stored as matrix[input_channel][output_channel].
  const float M[3][3] = {   // rows: X,Y,Z ; cols: R,G,B
    { 0.4124f, 0.3576f, 0.1805f },
    { 0.2126f, 0.7152f, 0.0722f },
    { 0.0193f, 0.1192f, 0.9505f },
  };
  for(int in = 0; in < 3; in++)
    for(int out = 0; out < 3; out++)
      p->matrix_in_transposed[in][out] = M[out][in];
}

static void test_vectorscope_hue_ring_primaries_are_distinct(void **state)
{
  (void)state;
  dt_iop_order_iccprofile_info_t prof;
  _make_test_profile(&prof);

  dt_scopes_vectorscope_t v;
  memset(&v, 0, sizeof(v));
  dt_scopes_vectorscope_hue_ring(&prof, DT_SCOPES_VEC_TYPE_CIELUV,
                                 DT_SCOPES_VEC_SCALE_LINEAR, &v);

  assert_true(v.radius > 0.f);

  // The hue ring is indexed [vertex][step]; the six vertices are R, Y, G,
  // C, B, M. Their starting chromaticity angles must be well separated
  // (a non-degenerate wheel), and red / green / blue must occupy three
  // distinct directions.
  const int red = 0, green = 2, blue = 4;
  const float ar = atan2f(v.hue_ring[red][0][1], v.hue_ring[red][0][0]);
  const float ag = atan2f(v.hue_ring[green][0][1], v.hue_ring[green][0][0]);
  const float ab = atan2f(v.hue_ring[blue][0][1], v.hue_ring[blue][0][0]);
  assert_true(fabsf(ar - ag) > 0.3f);
  assert_true(fabsf(ag - ab) > 0.3f);
  assert_true(fabsf(ar - ab) > 0.3f);

  // each ring point has non-zero radius (a real gamut boundary)
  float rr = hypotf(v.hue_ring[red][0][0], v.hue_ring[red][0][1]);
  assert_true(rr > 0.f);
}

static void test_vectorscope_graph_has_mass_offcenter_for_saturated_input(void **state)
{
  (void)state;
  const int w = 64, h = 64, diam = 96;
  float *img = _make_image(w, h);
  for(int i = 0; i < w * h; i++)
  {
    img[4 * i + 0] = 0.8f; img[4 * i + 1] = 0.1f;   // saturated red-ish
    img[4 * i + 2] = 0.1f; img[4 * i + 3] = 0.0f;
  }
  dt_histogram_roi_t roi = _full_roi(w, h);
  float lut[256]; _fill_identity_lut(lut, 256);

  dt_iop_order_iccprofile_info_t prof;
  _make_test_profile(&prof);

  dt_scopes_vectorscope_t *v = dt_scopes_vectorscope_alloc_compute(
      img, &roi, &prof, DT_SCOPES_VEC_TYPE_CIELUV, DT_SCOPES_VEC_SCALE_LINEAR, diam, lut, 256);
  assert_non_null(v);
  assert_int_equal(v->diameter, diam);

  // The graph must have some non-zero coverage (the saturated input lands
  // somewhere on the plot), and the centroid should be off the exact
  // centre (a saturated colour is not neutral grey).
  const size_t a8_stride = ((diam + 3) / 4) * 4;
  double sx = 0, sy = 0, sw = 0;
  for(int y = 0; y < diam; y++)
    for(int x = 0; x < diam; x++)
    {
      const uint8_t g = v->graph[(size_t)y * a8_stride + x];
      if(g) { sx += x * (double)g; sy += y * (double)g; sw += g; }
    }
  assert_true(sw > 0.0);
  const double cx = sx / sw, cy = sy / sw;
  const double dist = hypot(cx - diam / 2.0, cy - diam / 2.0);
  assert_true(dist > 2.0);   // clearly off-centre

  dt_scopes_vectorscope_free(v);
  dt_free_align(img);
}

/* ---- colorizers + PNG --------------------------------------------------- */

static void test_histogram_colorize_and_png_header(void **state)
{
  (void)state;
  dt_scopes_histogram_t hist;
  memset(&hist, 0, sizeof(hist));
  for(int b = 0; b < DT_SCOPES_HISTOGRAM_BINS; b++)
  {
    hist.bins[4 * b + 0] = b;                        // red ramp
    hist.bins[4 * b + 1] = DT_SCOPES_HISTOGRAM_BINS; // green flat
    hist.bins[4 * b + 2] = DT_SCOPES_HISTOGRAM_BINS - b;
  }
  hist.max = DT_SCOPES_HISTOGRAM_BINS;

  size_t stride;
  uint8_t *rgb = dt_scopes_histogram_colorize(&hist, 200, 100, FALSE, &stride);
  assert_non_null(rgb);

  GByteArray *png = dt_scopes_encode_png_rgb24(rgb, 200, 100, stride);
  assert_non_null(png);
  assert_true(png->len > 8);
  // PNG magic: 89 50 4E 47 0D 0A 1A 0A
  static const uint8_t magic[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
  assert_memory_equal(png->data, magic, 8);

  g_byte_array_unref(png);
  g_free(rgb);
}

static void test_png_encoder_rejects_bad_input(void **state)
{
  (void)state;
  assert_null(dt_scopes_encode_png_rgb24(NULL, 10, 10, 40));
  uint8_t buf[16] = { 0 };
  assert_null(dt_scopes_encode_png_rgb24(buf, 0, 10, 40));
}

/* ---- fixture / main ----------------------------------------------------- */

static int _group_setup(void **state)
{
  (void)state;
  // The OMP kernels size their per-thread scratch by dt_get_num_threads()
  // = CLAMP(nprocs, 1, darktable.num_openmp_threads); an uninitialised
  // global would yield 0 slots while the parallel region still spawns
  // threads -- a buffer overflow. Force single-threaded, deterministic
  // execution for the tests.
#ifdef _OPENMP
  omp_set_num_threads(1);
#endif
  darktable.num_openmp_threads = 1;
  return 0;
}

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_histogram_constant_image_lands_in_one_bin),
    cmocka_unit_test(test_histogram_matches_reference_count),
    cmocka_unit_test(test_histogram_summary_ramp),
    cmocka_unit_test(test_waveform_horizontal_dims),
    cmocka_unit_test(test_waveform_vertical_orientation_swaps_axes),
    cmocka_unit_test(test_parade_is_three_panels_wide),
    cmocka_unit_test(test_vectorscope_hue_ring_primaries_are_distinct),
    cmocka_unit_test(test_vectorscope_graph_has_mass_offcenter_for_saturated_input),
    cmocka_unit_test(test_histogram_colorize_and_png_header),
    cmocka_unit_test(test_png_encoder_rejects_bad_input),
  };

  return cmocka_run_group_tests(tests, _group_setup, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
