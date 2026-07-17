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

// The per-op band module adapter registry, mirroring
// remote_vector_registry.c's own split: static adapter table, registry
// lifecycle (lookup/validate), and the read-only half of the band engine
// API (list_schema/read_values). The adapter table registers lowlight
// and rawdenoise (Task 5), denoiseprofile (Task 6), and atrous (Task 7),
// the same way colorbalance/channelmixerrgb/rgblevels/borders/watermark
// were added to the vector registry one at a time.
//
// Like remote_band.c, this file never includes JSON, socket, or MCP
// protocol headers -- introspection/GLib (plus develop/imageop.h, for the
// dt_iop_module_so_t/dt_iop_module_t definitions the engine API's public
// signatures only forward-declare in remote_band.h) only.
//
// Threading: like remote_band.h/remote_vector.h/remote_curve.h/
// remote_edit.h, resolving a path against a live darkroom module's params
// block must happen from the GTK main thread. The static registry table
// and the adapter/version intrinsic validation cache below is read-mostly,
// written once per (adapter, params_version) the first time that pair is
// validated; like the rest of this subsystem, this file assumes single
// (main-context) threaded access and adds no locking of its own.

#include "control/remote_band.h"

#include "common/darktable.h" // _()
#include "control/remote_vector.h" // dt_remote_vector_registry_lookup(), for cross-class uniqueness
#include "develop/imageop.h" // dt_iop_module_so_t, dt_iop_module_t

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* error helper (mirrors remote_band.c's own file-local idiom -- see that  */
/* file's comment)                                                         */
/* ---------------------------------------------------------------------- */

static dt_remote_error_t *dt_remote_band_registry_error_new(dt_remote_error_code_t code,
                                                              const char *format, ...)
  G_GNUC_PRINTF(2, 3);

static dt_remote_error_t *dt_remote_band_registry_error_new(dt_remote_error_code_t code,
                                                              const char *format, ...)
{
  dt_remote_error_t *error = g_malloc0(sizeof(dt_remote_error_t));
  error->code = code;

  va_list args;
  va_start(args, format);
  error->message = g_strdup_vprintf(format, args);
  va_end(args);

  return error;
}

static void deliver_error(dt_remote_error_t *owned_error, dt_remote_error_t **out_error)
{
  if(out_error)
    *out_error = owned_error;
  else
    dt_remote_error_free(owned_error);
}

/* ---------------------------------------------------------------------- */
/* lowlight adapter (milestone5 bands-class design doc SS Adapters,        */
/* lowlight, as amended post-Task-6). Params v1: `transition_x[6]`/        */
/* `transition_y[6]` -- two 1-D float leaves (lowlight.c:47-49), defaults  */
/* x = k/5 (init(), lowlight.c:288), y = 0.5 ($DEFAULT). One semantic      */
/* band-set over them: y range [0,1] (the GUI drag clamp), INTERIOR x      */
/* policy with minimum gap 0.001 -- the GUI's x-drag strip below the       */
/* curve moves interior transition_x nodes with pinned endpoints and a     */
/* 0.001 at-least neighbor clamp (lowlight_motion_notify,                  */
/* lowlight.c:684-686), and the shipped "night blooming" preset stores a   */
/* non-default x (lowlight.c:424) -- no twins, no predicates: always       */
/* active, always writable.                                                */
/* ---------------------------------------------------------------------- */

static const dt_remote_path_segment_t s_lowlight_transition_x_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "transition_x" },
};
static const dt_remote_path_segment_t s_lowlight_transition_y_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "transition_y" },
};

static const dt_remote_band_descriptor_t s_lowlight_bands[1] = {
  {
    .name = "bands.transition",
    .display_name = "Transition",
    .description = "Day-to-night vision transition curve (lowlight.c:47-49): per-band blend "
                   "between photopic and scotopic response over the six fixed brightness bands.",
    .native_x = { .segments = s_lowlight_transition_x_segments,
                  .length = G_N_ELEMENTS(s_lowlight_transition_x_segments) },
    .native_y = { .segments = s_lowlight_transition_y_segments,
                  .length = G_N_ELEMENTS(s_lowlight_transition_y_segments) },
    .count = 6,
    .y_minimum = 0.0,
    .y_maximum = 1.0,
    .x_policy = DT_REMOTE_BAND_X_INTERIOR,
    .minimum_gap = 0.001,
    .x_shared_with = NULL,
    .active_when = NULL,
    .writable_when = NULL,
  },
};

static const dt_remote_band_module_adapter_t s_lowlight_adapter = {
  "lowlight", 1, 1, s_lowlight_bands, 1, NULL, 0, NULL
};

/* ---------------------------------------------------------------------- */
/* rawdenoise adapter (milestone5 bands-class design doc SS Adapters,      */
/* rawdenoise). Params v2: `x[4][5]`/`y[4][5]` channel rows                */
/* (rawdenoise.c:55-56; dt_iop_rawdenoise_channel_t: all=0, red=1,         */
/* green=2, blue=3), defaults x = k/4 (init(), rawdenoise.c:498), y = 0.5  */
/* ($DEFAULT). Four semantic band-sets, one per channel row: y range       */
/* [0,1] (the GUI drag clamp), FIXED x policy (the GUI never moves x), no  */
/* twins, no predicates: always active, always writable.                   */
/* ---------------------------------------------------------------------- */

static const dt_remote_path_segment_t s_rawdenoise_x0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_rawdenoise_y0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_rawdenoise_x1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_rawdenoise_y1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_rawdenoise_x2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_rawdenoise_y2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_rawdenoise_x3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};
static const dt_remote_path_segment_t s_rawdenoise_y3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};

// The four channel descriptors differ only in name/display/row -- shared
// invariants (count 5, y [0,1], FIXED) spelled once per entry to stay
// greppable static data, same convention as the vector registry's
// s_borders_vectors[].
#define RAWDENOISE_BAND_DESCRIPTOR(band_name, band_display, band_description, row)                          \
  {                                                                                                          \
    .name = (band_name), .display_name = (band_display), .description = (band_description),                  \
    .native_x = { .segments = s_rawdenoise_x##row##_segments,                                                \
                  .length = G_N_ELEMENTS(s_rawdenoise_x##row##_segments) },                                  \
    .native_y = { .segments = s_rawdenoise_y##row##_segments,                                                \
                  .length = G_N_ELEMENTS(s_rawdenoise_y##row##_segments) },                                  \
    .count = 5, .y_minimum = 0.0, .y_maximum = 1.0, .x_policy = DT_REMOTE_BAND_X_FIXED,                      \
    .minimum_gap = 0.0, .x_shared_with = NULL, .active_when = NULL, .writable_when = NULL,                   \
  }

static const dt_remote_band_descriptor_t s_rawdenoise_bands[4] = {
  RAWDENOISE_BAND_DESCRIPTOR("bands.all", "All",
                             "Wavelet noise-threshold curve applied to all channels "
                             "(rawdenoise.c:55-56, row 0).", 0),
  RAWDENOISE_BAND_DESCRIPTOR("bands.red", "Red",
                             "Wavelet noise-threshold curve for the red channel (row 1).", 1),
  RAWDENOISE_BAND_DESCRIPTOR("bands.green", "Green",
                             "Wavelet noise-threshold curve for the green channel (row 2).", 2),
  RAWDENOISE_BAND_DESCRIPTOR("bands.blue", "Blue",
                             "Wavelet noise-threshold curve for the blue channel (row 3).", 3),
};

static const dt_remote_band_module_adapter_t s_rawdenoise_adapter = {
  "rawdenoise", 2, 2, s_rawdenoise_bands, 4, NULL, 0, NULL
};

/* ---------------------------------------------------------------------- */
/* denoiseprofile adapter (milestone5 bands-class design doc SS Adapters,  */
/* denoiseprofile). Params v12: `x[6][7]`/`y[6][7]` channel rows           */
/* (denoiseprofile.c:119-121; dt_iop_denoiseprofile_channel_t: all=0,      */
/* R=1, G=2, B=3, Y0=4, U0V0=5), defaults x = k/6 (init(),                 */
/* denoiseprofile.c:2650), y = 0.5 ($DEFAULT). Six semantic band-sets,     */
/* one per channel row: y range [0,1] (the GUI drag clamp), FIXED x        */
/* policy, no twins, no predicates -- all six channels are always          */
/* writable: the GUI stores every channel row regardless of the active     */
/* wavelet_color_mode, so remote writes to an inactive-mode channel are    */
/* exactly as meaningful as GUI edits made before switching modes. The     */
/* noise-fit a[3]/b[3] arrays stay excluded (denylisted since milestone    */
/* 2). No byte poking anywhere: only introspection paths touch the v12     */
/* blob, and registry_validate's resolved-length==count check guards the   */
/* layout (the design doc's open padding question resolves to "not our     */
/* problem").                                                              */
/* ---------------------------------------------------------------------- */

static const dt_remote_path_segment_t s_denoiseprofile_x0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_denoiseprofile_x1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_denoiseprofile_x2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_denoiseprofile_x3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};
static const dt_remote_path_segment_t s_denoiseprofile_x4_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 4 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y4_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 4 },
};
static const dt_remote_path_segment_t s_denoiseprofile_x5_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 5 },
};
static const dt_remote_path_segment_t s_denoiseprofile_y5_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 5 },
};

// Same shared-invariants-per-row convention as RAWDENOISE_BAND_DESCRIPTOR
// above, with denoiseprofile's count of 7.
#define DENOISEPROFILE_BAND_DESCRIPTOR(band_name, band_display, band_description, row)                       \
  {                                                                                                          \
    .name = (band_name), .display_name = (band_display), .description = (band_description),                  \
    .native_x = { .segments = s_denoiseprofile_x##row##_segments,                                            \
                  .length = G_N_ELEMENTS(s_denoiseprofile_x##row##_segments) },                              \
    .native_y = { .segments = s_denoiseprofile_y##row##_segments,                                            \
                  .length = G_N_ELEMENTS(s_denoiseprofile_y##row##_segments) },                              \
    .count = 7, .y_minimum = 0.0, .y_maximum = 1.0, .x_policy = DT_REMOTE_BAND_X_FIXED,                      \
    .minimum_gap = 0.0, .x_shared_with = NULL, .active_when = NULL, .writable_when = NULL,                   \
  }

static const dt_remote_band_descriptor_t s_denoiseprofile_bands[6] = {
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.all", "All",
                                 "Wavelet force curve applied to all channels "
                                 "(denoiseprofile.c:119-121, row 0).", 0),
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.red", "Red",
                                 "Wavelet force curve for the red channel (RGB color mode, row 1).", 1),
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.green", "Green",
                                 "Wavelet force curve for the green channel (RGB color mode, row 2).", 2),
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.blue", "Blue",
                                 "Wavelet force curve for the blue channel (RGB color mode, row 3).", 3),
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.y0", "Luminance (Y0)",
                                 "Wavelet force curve for the Y0 luminance channel (Y0U0V0 color "
                                 "mode, row 4).", 4),
  DENOISEPROFILE_BAND_DESCRIPTOR("bands.u0v0", "Chrominance (U0V0)",
                                 "Wavelet force curve for the U0V0 chrominance channels (Y0U0V0 "
                                 "color mode, row 5).", 5),
};

static const dt_remote_band_module_adapter_t s_denoiseprofile_adapter = {
  "denoiseprofile", 12, 12, s_denoiseprofile_bands, 6, NULL, 0, NULL
};

/* ---------------------------------------------------------------------- */
/* atrous adapter (milestone5 bands-class design doc SS Adapters, atrous). */
/* Params v2: `x[5][6]`/`y[5][6]` channel rows (atrous.c:77-78,            */
/* atrous_channel_t: L=0, c=1, s=2, Lt=3, ct=4), defaults x = k/5          */
/* (init(), atrous.c:618), y = 0.5 ($DEFAULT) for the boost rows but 0.0   */
/* for the threshold rows (init() override, atrous.c:616). Five semantic   */
/* band-sets, one per channel row: y range [0,1] (the GUI drag clamp),     */
/* INTERIOR x policy with minimum gap 0.001 (the GUI's x-drag clamp,       */
/* atrous.c:1404-1408), and twin x sharing exactly as the GUI mirrors an   */
/* x drag across its boost/threshold row pair (`ch2`, atrous.c:1393-1395,  */
/* 1408): luma<->luma_threshold and chroma<->chroma_threshold; sharpness   */
/* has no twin. No predicates: always active, always writable. The         */
/* `octaves` scalar is denylisted in remote_edit.c (auto-derived in        */
/* commit_params, atrous.c:684); `mix` stays an ordinary writable scalar.  */
/* ---------------------------------------------------------------------- */

static const dt_remote_path_segment_t s_atrous_x0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_atrous_y0_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 0 },
};
static const dt_remote_path_segment_t s_atrous_x1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_atrous_y1_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 1 },
};
static const dt_remote_path_segment_t s_atrous_x2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_atrous_y2_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 2 },
};
static const dt_remote_path_segment_t s_atrous_x3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};
static const dt_remote_path_segment_t s_atrous_y3_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 3 },
};
static const dt_remote_path_segment_t s_atrous_x4_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "x" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 4 },
};
static const dt_remote_path_segment_t s_atrous_y4_segments[] = {
  { .type = DT_REMOTE_PATH_FIELD, .value.field = "y" },
  { .type = DT_REMOTE_PATH_INDEX, .value.index = 4 },
};

// Same shared-invariants-per-row convention as the two macros above, with
// atrous's count of 6, INTERIOR x, and a per-row twin link (NULL for the
// twinless sharpness row).
#define ATROUS_BAND_DESCRIPTOR(band_name, band_display, band_description, row, twin)                         \
  {                                                                                                          \
    .name = (band_name), .display_name = (band_display), .description = (band_description),                  \
    .native_x = { .segments = s_atrous_x##row##_segments,                                                    \
                  .length = G_N_ELEMENTS(s_atrous_x##row##_segments) },                                      \
    .native_y = { .segments = s_atrous_y##row##_segments,                                                    \
                  .length = G_N_ELEMENTS(s_atrous_y##row##_segments) },                                      \
    .count = 6, .y_minimum = 0.0, .y_maximum = 1.0, .x_policy = DT_REMOTE_BAND_X_INTERIOR,                   \
    .minimum_gap = 0.001, .x_shared_with = (twin), .active_when = NULL, .writable_when = NULL,               \
  }

static const dt_remote_band_descriptor_t s_atrous_bands[5] = {
  ATROUS_BAND_DESCRIPTOR("bands.luma", "Luma",
                         "Contrast-equalizer luminance boost curve (atrous.c:77-78, row 0); "
                         "shares its x positions with bands.luma_threshold.",
                         0, "bands.luma_threshold"),
  ATROUS_BAND_DESCRIPTOR("bands.chroma", "Chroma",
                         "Contrast-equalizer chrominance boost curve (row 1); shares its x "
                         "positions with bands.chroma_threshold.",
                         1, "bands.chroma_threshold"),
  ATROUS_BAND_DESCRIPTOR("bands.sharpness", "Sharpness",
                         "Contrast-equalizer edge sharpness curve (row 2); its x positions are "
                         "shared with no other band-set.",
                         2, NULL),
  ATROUS_BAND_DESCRIPTOR("bands.luma_threshold", "Luma threshold",
                         "Contrast-equalizer luminance noise threshold curve (row 3); shares its "
                         "x positions with bands.luma.",
                         3, "bands.luma"),
  ATROUS_BAND_DESCRIPTOR("bands.chroma_threshold", "Chroma threshold",
                         "Contrast-equalizer chrominance noise threshold curve (row 4); shares "
                         "its x positions with bands.chroma.",
                         4, "bands.chroma"),
};

static const dt_remote_band_module_adapter_t s_atrous_adapter = {
  "atrous", 2, 2, s_atrous_bands, 5, NULL, 0, NULL
};

/* ---------------------------------------------------------------------- */
/* adapter table                                                           */
/* ---------------------------------------------------------------------- */

// lowlight and rawdenoise (Task 5), denoiseprofile (Task 6), atrous
// (Task 7), the same convention as remote_vector_registry.c's own
// s_adapters[] (and remote_curve_registry.c's before it): a future op adds
// another entry here, not a parallel lookup mechanism.
static const dt_remote_band_module_adapter_t *const s_adapters[] = {
  &s_lowlight_adapter,
  &s_rawdenoise_adapter,
  &s_denoiseprofile_adapter,
  &s_atrous_adapter,
};

static dt_remote_band_registry_lookup_override_t s_lookup_override = NULL;

void dt_remote_band_registry_set_lookup_override(dt_remote_band_registry_lookup_override_t lookup)
{
  s_lookup_override = lookup;
}

/* ---------------------------------------------------------------------- */
/* registry lifecycle                                                      */
/* ---------------------------------------------------------------------- */

const dt_remote_band_module_adapter_t *
dt_remote_band_registry_lookup(const char *operation, guint params_version)
{
  if(!operation) return NULL;
  if(s_lookup_override) return s_lookup_override(operation, params_version);

  for(guint i = 0; i < G_N_ELEMENTS(s_adapters); i++)
  {
    const dt_remote_band_module_adapter_t *adapter = s_adapters[i];
    if(!adapter) continue;
    if(!g_strcmp0(adapter->operation, operation)
       && params_version >= adapter->minimum_params_version
       && params_version <= adapter->maximum_params_version)
      return adapter;
  }

  return NULL;
}

// Process-lifetime intrinsic validation cache, keyed by adapter pointer
// identity and the introspection params version -- same convention as
// remote_vector_registry.c's own cache. Cross-class uniqueness is mutable
// under the registry lookup overrides and is therefore not cached.
typedef struct dt_remote_band_registry_cache_key_t
{
  const dt_remote_band_module_adapter_t *adapter;
  int params_version;
} dt_remote_band_registry_cache_key_t;

typedef struct dt_remote_band_registry_cache_entry_t
{
  gboolean adapter_valid; // TRUE iff every intrinsic check passed for this version
} dt_remote_band_registry_cache_entry_t;

static GHashTable *s_validation_cache = NULL; // cache key -> intrinsic result; never freed
                                              // (process lifetime, like the static descriptors/
                                              // adapters it caches results for)

static guint validation_cache_key_hash(gconstpointer data)
{
  const dt_remote_band_registry_cache_key_t *key = data;
  return g_direct_hash((gpointer)key->adapter) ^ g_int_hash(&key->params_version);
}

static gboolean validation_cache_key_equal(gconstpointer left, gconstpointer right)
{
  const dt_remote_band_registry_cache_key_t *a = left;
  const dt_remote_band_registry_cache_key_t *b = right;
  return a->adapter == b->adapter && a->params_version == b->params_version;
}

static dt_remote_band_registry_cache_entry_t *
lookup_cache_entry(const dt_remote_band_module_adapter_t *adapter, int params_version)
{
  if(!s_validation_cache)
    s_validation_cache = g_hash_table_new_full(validation_cache_key_hash, validation_cache_key_equal,
                                               g_free, g_free);

  const dt_remote_band_registry_cache_key_t key = {
    .adapter = adapter,
    .params_version = params_version,
  };
  return g_hash_table_lookup(s_validation_cache, &key);
}

static void cache_validation_result(const dt_remote_band_module_adapter_t *adapter,
                                    int params_version,
                                    gboolean adapter_valid)
{
  dt_remote_band_registry_cache_key_t *key = g_new(dt_remote_band_registry_cache_key_t, 1);
  key->adapter = adapter;
  key->params_version = params_version;

  dt_remote_band_registry_cache_entry_t *entry = g_new(dt_remote_band_registry_cache_entry_t, 1);
  entry->adapter_valid = adapter_valid;
  g_hash_table_insert(s_validation_cache, key, entry);
}

static gboolean band_predicate_is_valid(const dt_remote_parameter_predicate_t *predicate,
                                        const dt_introspection_field_t *root,
                                        void *dummy_blob)
{
  if(!predicate) return TRUE;

  if(predicate->op != DT_REMOTE_PREDICATE_EQ && predicate->op != DT_REMOTE_PREDICATE_NE)
    return FALSE;

  dt_introspection_field_t *field = NULL;
  if(!dt_introspection_get_child((dt_introspection_field_t *)root, dummy_blob,
                                 predicate->field, &field)
     || !field
     || field->header.type != DT_INTROSPECTION_TYPE_ENUM
     || !field->Enum.values)
    return FALSE;

  int unused_value = 0;
  return dt_introspection_get_enum_value(field, predicate->enum_name, &unused_value);
}

static gboolean nonempty_string(const char *value)
{
  return value && value[0] != '\0';
}

static gboolean band_adapter_envelope_is_valid(
  const dt_remote_band_module_adapter_t *adapter,
  const dt_iop_module_so_t *so,
  const dt_introspection_t *intro)
{
  if(!nonempty_string(adapter->operation)
     || g_strcmp0(adapter->operation, so->op)
     || adapter->minimum_params_version > adapter->maximum_params_version
     || intro->params_version < 0
     || (guint)intro->params_version < adapter->minimum_params_version
     || (guint)intro->params_version > adapter->maximum_params_version
     || (adapter->band_count > 0 && !adapter->bands)
     || (adapter->prepare_field_count > 0 && !adapter->prepare_fields))
    return FALSE;

  for(guint i = 0; i < adapter->prepare_field_count; i++)
    if(!nonempty_string(adapter->prepare_fields[i])) return FALSE;
  return TRUE;
}

static gboolean band_descriptor_metadata_is_valid(const dt_remote_band_descriptor_t *desc)
{
  if(!nonempty_string(desc->name) || desc->count == 0) return FALSE;

  if(!isfinite(desc->y_minimum) || !isfinite(desc->y_maximum) || desc->y_minimum > desc->y_maximum)
    return FALSE;
  // A finite double bound outside the native float range would let a
  // candidate at that same bound pass dt_remote_band_validate()'s domain
  // check and then narrow to +-inf in the write path's `(float)value`
  // conversion -- same rationale as vector_component_metadata_is_valid()
  // in remote_vector_registry.c.
  if(desc->y_minimum < -(double)FLT_MAX || desc->y_minimum > (double)FLT_MAX
     || desc->y_maximum < -(double)FLT_MAX || desc->y_maximum > (double)FLT_MAX)
    return FALSE;

  switch(desc->x_policy)
  {
    case DT_REMOTE_BAND_X_FIXED:
      return desc->minimum_gap == 0.0;
    case DT_REMOTE_BAND_X_INTERIOR:
      return isfinite(desc->minimum_gap) && desc->minimum_gap > 0.0;
    default:
      return FALSE;
  }
}

// Checks one native leaf (native_x or native_y): a single fixed-capacity
// float array leaf whose resolved length equals `desc->count` exactly --
// unlike a vector's native_capacity, a band has no preserved tail, so
// "at least component_count" (vector_descriptor_is_valid()'s own rule)
// tightens to "exactly count" here.
static gboolean band_native_array_is_valid(const dt_remote_band_descriptor_t *desc,
                                           const dt_remote_introspection_path_t *path,
                                           const dt_introspection_field_t *root,
                                           void *dummy_blob)
{
  if(path->length && !path->segments) return FALSE;

  const dt_introspection_field_t *array_field = NULL;
  if(!dt_remote_path_resolve(path, root, dummy_blob, &array_field, NULL, NULL)) return FALSE;
  if(!array_field
     || array_field->header.type != DT_INTROSPECTION_TYPE_ARRAY
     || array_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT
     || !array_field->Array.field
     || array_field->Array.field->header.type != DT_INTROSPECTION_TYPE_FLOAT
     || array_field->Array.field->header.size != sizeof(float))
    return FALSE;
  if(array_field->Array.count != (size_t)desc->count) return FALSE;
  // Guard the multiply against overflow before computing it -- same
  // rationale as vector_descriptor_is_valid()'s matching check.
  if(array_field->Array.count > SIZE_MAX / sizeof(float)) return FALSE;
  if(array_field->header.size != array_field->Array.count * sizeof(float)) return FALSE;

  return TRUE;
}

// Checks one descriptor's native layout (two independent fixed-capacity
// float array leaves) and optional predicates against `root` (the module's
// whole params struct field, DT_INTROSPECTION_TYPE_STRUCT). `dummy_blob`
// exists only to satisfy the introspection cursor signatures; validation
// never reads its pointee.
static gboolean band_descriptor_is_valid(const dt_remote_band_descriptor_t *desc,
                                         const dt_introspection_field_t *root,
                                         void *dummy_blob)
{
  if(!band_descriptor_metadata_is_valid(desc)) return FALSE;
  if(root->header.type != DT_INTROSPECTION_TYPE_STRUCT) return FALSE;

  if(!band_native_array_is_valid(desc, &desc->native_x, root, dummy_blob)) return FALSE;
  if(!band_native_array_is_valid(desc, &desc->native_y, root, dummy_blob)) return FALSE;

  return band_predicate_is_valid(desc->active_when, root, dummy_blob)
         && band_predicate_is_valid(desc->writable_when, root, dummy_blob);
}

// Twin symmetry: when `desc->x_shared_with` is non-NULL, it must name
// another descriptor on the same adapter (self-reference is not rejected;
// it trivially satisfies symmetry), that descriptor's `count` must match,
// and that descriptor's own `x_shared_with` must point back to `desc`.
static gboolean band_twin_metadata_is_valid(const dt_remote_band_module_adapter_t *adapter,
                                            const dt_remote_band_descriptor_t *desc)
{
  if(!desc->x_shared_with) return TRUE;

  const dt_remote_band_descriptor_t *twin = NULL;
  for(guint i = 0; i < adapter->band_count; i++)
    if(!g_strcmp0(adapter->bands[i].name, desc->x_shared_with))
    {
      twin = &adapter->bands[i];
      break;
    }
  if(!twin) return FALSE;
  if(twin->count != desc->count) return FALSE;
  if(g_strcmp0(twin->x_shared_with, desc->name)) return FALSE;

  return TRUE;
}

static gboolean band_adapter_has_duplicate_names(const dt_remote_band_module_adapter_t *adapter,
                                                  const char **out_name)
{
  for(guint i = 0; i < adapter->band_count; i++)
    for(guint j = 0; j < i; j++)
      if(!g_strcmp0(adapter->bands[i].name, adapter->bands[j].name))
      {
        if(out_name) *out_name = adapter->bands[i].name;
        return TRUE;
      }
  return FALSE;
}

// Cross-class uniqueness: no band semantic ID on `adapter` may collide
// with a curve or vector semantic ID registered for the same (operation,
// params_version) pair. Returns TRUE (and, if non-NULL, sets
// *out_collision_name) on the first collision found.
static gboolean band_names_collide_with_other_classes(const dt_remote_band_module_adapter_t *adapter,
                                                       guint params_version,
                                                       const char **out_collision_name)
{
  const dt_remote_curve_module_adapter_t *curve_adapter =
    dt_remote_curve_registry_lookup(adapter->operation, params_version);
  if(curve_adapter)
    for(guint i = 0; i < adapter->band_count; i++)
      for(guint j = 0; j < curve_adapter->curve_count; j++)
        if(!g_strcmp0(adapter->bands[i].name, curve_adapter->curves[j].name))
        {
          if(out_collision_name) *out_collision_name = adapter->bands[i].name;
          return TRUE;
        }

  const dt_remote_vector_module_adapter_t *vector_adapter =
    dt_remote_vector_registry_lookup(adapter->operation, params_version);
  if(vector_adapter)
    for(guint i = 0; i < adapter->band_count; i++)
      for(guint j = 0; j < vector_adapter->vector_count; j++)
        if(!g_strcmp0(adapter->bands[i].name, vector_adapter->vectors[j].name))
        {
          if(out_collision_name) *out_collision_name = adapter->bands[i].name;
          return TRUE;
        }

  return FALSE;
}

gboolean dt_remote_band_registry_validate(const dt_remote_band_module_adapter_t *adapter,
                                          const struct dt_iop_module_so_t *so,
                                          dt_remote_error_t **error)
{
  if(!adapter || !so)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("internal error: null adapter or module passed to band registry validation"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  if(!band_adapter_envelope_is_valid(adapter, so, intro))
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL,
        _("band adapter metadata does not match module '%s' introspection version %d"),
        so->op, intro->params_version);
    return FALSE;
  }

  dt_remote_band_registry_cache_entry_t *cache =
    lookup_cache_entry(adapter, intro->params_version);
  if(!cache)
  {
    gboolean intrinsic_valid = TRUE;
    const char *first_failure = NULL;
    if(band_adapter_has_duplicate_names(adapter, &first_failure)) intrinsic_valid = FALSE;
    guint8 dummy_byte = 0;
    for(guint i = 0; i < adapter->band_count; i++)
    {
      const dt_remote_band_descriptor_t *desc = &adapter->bands[i];
      if(!band_descriptor_is_valid(desc, intro->field, &dummy_byte)
         || !band_twin_metadata_is_valid(adapter, desc))
      {
        intrinsic_valid = FALSE;
        if(!first_failure) first_failure = desc->name;
      }
    }
    cache_validation_result(adapter, intro->params_version, intrinsic_valid);
    cache = lookup_cache_entry(adapter, intro->params_version);

    if(!intrinsic_valid)
    {
      deliver_error(dt_remote_band_registry_error_new(
                      DT_REMOTE_ERR_INTERNAL,
                      _("band adapter '%s' descriptor '%s' does not match introspection version %d"),
                      adapter->operation, first_failure ? first_failure : "", intro->params_version),
                    error);
      return FALSE;
    }
  }

  if(!cache->adapter_valid)
  {
    deliver_error(dt_remote_band_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("band adapter '%s' does not match introspection version %d"),
                    adapter->operation, intro->params_version),
                  error);
    return FALSE;
  }

  const char *collision_name = NULL;
  if(band_names_collide_with_other_classes(adapter, (guint)intro->params_version, &collision_name))
  {
    deliver_error(dt_remote_band_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("semantic band '%s' collides with a curve or vector ID"),
                    collision_name ? collision_name : ""),
                  error);
    return FALSE;
  }
  return TRUE;
}

/* ---------------------------------------------------------------------- */
/* band engine API -- read-only half (list_schema/read_values)             */
/* ---------------------------------------------------------------------- */

static dt_remote_parameter_condition_t *condition_from_predicate(const dt_remote_parameter_predicate_t *pred)
{
  if(!pred) return NULL;
  dt_remote_parameter_condition_t *condition = g_new0(dt_remote_parameter_condition_t, 1);
  condition->field = g_strdup(pred->field);
  condition->op = pred->op;
  condition->enum_name = g_strdup(pred->enum_name);
  return condition;
}

gboolean dt_remote_band_list_schema(const struct dt_iop_module_so_t *so,
                                    GPtrArray **out_fields,
                                    dt_remote_error_t **error)
{
  if(out_fields) *out_fields = NULL;
  if(!so || !out_fields)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to band schema listing"));
    return FALSE;
  }

  dt_introspection_t *intro = so->get_introspection ? so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), so->op);
    return FALSE;
  }

  const dt_remote_band_module_adapter_t *adapter =
    dt_remote_band_registry_lookup(so->op, (guint)intro->params_version);
  if(!adapter)
  {
    // No adapter for this op/version: no semantic band-set advertised, not
    // an error (native array fields simply remain unsupported elsewhere).
    *out_fields = NULL;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_band_registry_validate(adapter, so, &validate_error);
  if(!adapter_valid)
  {
    if(!validate_error)
      validate_error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: band registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)dt_remote_band_schema_free);

  for(guint i = 0; i < adapter->band_count; i++)
  {
    const dt_remote_band_descriptor_t *desc = &adapter->bands[i];
    dt_remote_band_schema_t *schema = g_new0(dt_remote_band_schema_t, 1);

    schema->name = g_strdup(desc->name);
    schema->display_name = g_strdup(desc->display_name ? desc->display_name : "");
    schema->description = desc->description ? g_strdup(desc->description) : NULL;
    schema->count = desc->count;
    schema->y_minimum = desc->y_minimum;
    schema->y_maximum = desc->y_maximum;
    schema->x_policy = desc->x_policy;
    schema->minimum_gap = desc->minimum_gap;
    schema->x_shared_with = desc->x_shared_with ? g_strdup(desc->x_shared_with) : NULL;
    // No live params blob at this (per-op, `so`-only) level to resolve
    // current x positions from -- see dt_remote_band_list_schema()'s own
    // doc comment in remote_band.h.
    schema->x = NULL;
    schema->writability = desc->writable_when ? DT_REMOTE_WRITABLE_CONDITIONAL : DT_REMOTE_WRITABLE_NOW;
    schema->active_when = condition_from_predicate(desc->active_when);
    schema->writable_when = condition_from_predicate(desc->writable_when);

    g_ptr_array_add(result, schema);
  }

  *out_fields = result;
  return TRUE;
}

// Registry validation (band_native_array_is_valid() above) and the write
// path (write_band_patch(), remote_band.c) both require the native leaf to
// be strict FLOAT -- a DOUBLE-typed array field never passes registry
// validation, so this is only ever called with a FLOAT leaf. Read path,
// validate, and write all agree on strict FLOAT.
static double read_float_leaf(const dt_introspection_field_t *field, const void *ptr)
{
  (void)field;
  return (double)*(const float *)ptr;
}

// Evaluates `pred` against `params_blob`, resolving `pred->field` as a one
// segment path off `root` and comparing by enum *name*, never by raw
// integer -- same contract as remote_vector_registry.c's own
// predicate_holds() (this file keeps its own copy rather than sharing a
// static function across translation units, same established idiom as the
// curve/vector twins).
static gboolean predicate_holds(const dt_remote_parameter_predicate_t *pred,
                                const dt_introspection_field_t *root,
                                void *params_blob,
                                gboolean *out_holds,
                                dt_remote_error_t **error)
{
  const dt_remote_path_segment_t segment = { .type = DT_REMOTE_PATH_FIELD, .value.field = pred->field };
  const dt_remote_introspection_path_t path = { .segments = &segment, .length = 1 };

  const dt_introspection_field_t *field = NULL;
  void *ptr = NULL;
  dt_remote_error_t *resolve_error = NULL;
  if(!dt_remote_path_resolve(&path, root, params_blob, &field, &ptr, &resolve_error))
  {
    if(!resolve_error)
      resolve_error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: band predicate field '%s' did not resolve"),
        pred->field);
    deliver_error(resolve_error, error);
    return FALSE;
  }
  if(field->header.type != DT_INTROSPECTION_TYPE_ENUM)
  {
    deliver_error(dt_remote_band_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: band predicate field '%s' is not an enum"), pred->field),
                  error);
    return FALSE;
  }

  const int value = *(const int *)ptr;
  const char *name = dt_introspection_get_enum_name((dt_introspection_field_t *)field, value);
  if(!name)
  {
    deliver_error(dt_remote_band_registry_error_new(
                    DT_REMOTE_ERR_INTERNAL,
                    _("internal error: band predicate field '%s' has unknown enum value %d"),
                    pred->field, value),
                  error);
    return FALSE;
  }

  const gboolean matches = !g_strcmp0(name, pred->enum_name);
  *out_holds = (pred->op == DT_REMOTE_PREDICATE_EQ) ? matches : !matches;
  return TRUE;
}

gboolean dt_remote_band_read_values(const struct dt_iop_module_t *module,
                                    const void *params,
                                    GHashTable **out,
                                    dt_remote_error_t **error)
{
  if(out) *out = NULL;
  if(!module || !params || !out)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: null argument to band value read"));
    return FALSE;
  }

  if(!module->so)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module instance has no .so"));
    return FALSE;
  }

  dt_introspection_t *intro = module->so->get_introspection ? module->so->get_introspection() : NULL;
  if(!intro || !intro->field)
  {
    if(error)
      *error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: module '%s' has no introspection"), module->op);
    return FALSE;
  }

  GHashTable *result =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)dt_remote_band_value_free);

  const dt_remote_band_module_adapter_t *adapter =
    dt_remote_band_registry_lookup(module->op, (guint)intro->params_version);
  if(!adapter)
  {
    *out = result;
    return TRUE;
  }

  dt_remote_error_t *validate_error = NULL;
  const gboolean adapter_valid = dt_remote_band_registry_validate(adapter, module->so, &validate_error);
  if(!adapter_valid)
  {
    g_hash_table_destroy(result);
    if(!validate_error)
      validate_error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: band registry validation failed"));
    deliver_error(validate_error, error);
    return FALSE;
  }
  if(validate_error) dt_remote_error_free(validate_error);

  void *params_blob = (void *)params;
  dt_remote_error_t *read_error = NULL;

  for(guint i = 0; i < adapter->band_count; i++)
  {
    const dt_remote_band_descriptor_t *desc = &adapter->bands[i];

    gboolean active = TRUE;
    if(desc->active_when
       && !predicate_holds(desc->active_when, intro->field, params_blob, &active, &read_error))
      goto fail;

    gboolean writable_now = TRUE;
    if(desc->writable_when
       && !predicate_holds(desc->writable_when, intro->field, params_blob, &writable_now, &read_error))
      goto fail;

    const dt_introspection_field_t *y_field = NULL;
    void *y_ptr = NULL;
    if(!dt_remote_path_resolve(&desc->native_y, intro->field, params_blob, &y_field, &y_ptr, &read_error))
      goto fail;
    const dt_introspection_field_t *x_field = NULL;
    void *x_ptr = NULL;
    if(!dt_remote_path_resolve(&desc->native_x, intro->field, params_blob, &x_field, &x_ptr, &read_error))
      goto fail;
    if(y_field->header.type != DT_INTROSPECTION_TYPE_ARRAY || y_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT
       || x_field->header.type != DT_INTROSPECTION_TYPE_ARRAY || x_field->Array.type != DT_INTROSPECTION_TYPE_FLOAT)
    {
      read_error = dt_remote_band_registry_error_new(
        DT_REMOTE_ERR_INTERNAL, _("internal error: band '%s' native path did not resolve"), desc->name);
      goto fail;
    }

    GArray *y_values = g_array_sized_new(FALSE, FALSE, sizeof(double), desc->count);
    GArray *x_values = g_array_sized_new(FALSE, FALSE, sizeof(double), desc->count);
    gboolean component_failed = FALSE;
    for(guint j = 0; j < desc->count; j++)
    {
      dt_introspection_field_t *y_element_field = NULL;
      void *y_element_ptr =
        dt_introspection_access_array((dt_introspection_field_t *)y_field, y_ptr, j, &y_element_field);
      dt_introspection_field_t *x_element_field = NULL;
      void *x_element_ptr =
        dt_introspection_access_array((dt_introspection_field_t *)x_field, x_ptr, j, &x_element_field);
      if(!y_element_ptr || !y_element_field || y_element_field->header.type != DT_INTROSPECTION_TYPE_FLOAT
         || !x_element_ptr || !x_element_field || x_element_field->header.type != DT_INTROSPECTION_TYPE_FLOAT)
      {
        read_error = dt_remote_band_registry_error_new(
          DT_REMOTE_ERR_INTERNAL, _("internal error: band '%s' component %u did not resolve"),
          desc->name, j);
        component_failed = TRUE;
        break;
      }
      const double y_value = read_float_leaf(y_element_field, y_element_ptr);
      const double x_value = read_float_leaf(x_element_field, x_element_ptr);
      g_array_append_val(y_values, y_value);
      g_array_append_val(x_values, x_value);
    }
    if(component_failed)
    {
      g_array_unref(y_values);
      g_array_unref(x_values);
      goto fail;
    }

    dt_remote_band_value_t *value = g_new0(dt_remote_band_value_t, 1);
    value->name = g_strdup(desc->name);
    value->y = y_values;
    value->x = x_values;
    value->active = active;
    value->effective = active;
    value->writable_now = writable_now;

    g_hash_table_insert(result, g_strdup(desc->name), value);
  }

  *out = result;
  return TRUE;

fail:
  g_hash_table_destroy(result);
  if(!read_error)
    read_error = dt_remote_band_registry_error_new(
      DT_REMOTE_ERR_INTERNAL, _("internal error: band value read failed"));
  deliver_error(read_error, error);
  return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
