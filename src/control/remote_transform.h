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

// Shared preview<->raw coordinate mapping + the pipe-freshness contract
// (drawn-masks design amendment 1, shared verbatim with sample_region).
// Points are exact (dt_dev_distort_[back]transform on the live preview
// pipe); scalar sizes/angles use the documented probe algorithm and are
// exact only under uniform distortion. Every function taking a dt_develop_t
// must run on the GTK main thread (same rule as the masks GUI handlers).

#pragma once

#include "control/remote_edit.h"

#include <glib.h>

G_BEGIN_DECLS

struct dt_develop_t;

#define DT_REMOTE_TRANSFORM_SPREAD 0.01     // size_mapping "approximate" threshold
#define DT_REMOTE_TRANSFORM_ANGLE_ARM 0.05  // angle arm as a fraction of MIN(w,h), isotropic px

/** Block-until-clean freshness contract: TRUE when dev->preview_pipe->status
 * is DT_DEV_PIXELPIPE_VALID (waiting up to pixelpipe_synchronization_timeout
 * iterations of 5ms while enqueuing one preview reprocess); FALSE with
 * *error = DT_REMOTE_ERR_PIPE_NOT_READY (wire "retry_later") on INVALID or
 * timeout. A non-positive conf uses the 2000-iteration fallback. */
gboolean dt_remote_transform_ensure_fresh(struct dt_develop_t *dev,
                                          dt_remote_error_t **error);

void dt_remote_transform_preview_to_raw_point(struct dt_develop_t *dev,
                                              double px,
                                              double py,
                                              double *rx,
                                              double *ry);
void dt_remote_transform_raw_to_preview_point(struct dt_develop_t *dev,
                                              double rx,
                                              double ry,
                                              double *px,
                                              double *py);

/** Preview radius, expressed as a fraction of MIN(processed_width,
 * processed_height), at (cx_prev,cy_prev) -> raw-normalized length (raw pixel
 * distance / MIN(iwidth,iheight)). Probe arms are isotropic pixel offsets.
 * *exact_out is FALSE when the four transformed arm distances spread by more
 * than DT_REMOTE_TRANSFORM_SPREAD (relative). Returns FALSE only if the
 * underlying distort call fails. */
gboolean dt_remote_transform_preview_to_raw_size(struct dt_develop_t *dev,
                                                 double cx_prev,
                                                 double cy_prev,
                                                 double r_prev,
                                                 double *r_raw_out,
                                                 gboolean *exact_out);
gboolean dt_remote_transform_raw_to_preview_size(struct dt_develop_t *dev,
                                                 double cx_raw,
                                                 double cy_raw,
                                                 double r_raw,
                                                 double *r_prev_out,
                                                 gboolean *exact_out);

gboolean dt_remote_transform_preview_to_raw_angle(struct dt_develop_t *dev,
                                                  double cx_prev,
                                                  double cy_prev,
                                                  double deg_prev,
                                                  double *deg_raw_out);
gboolean dt_remote_transform_raw_to_preview_angle(struct dt_develop_t *dev,
                                                  double cx_raw,
                                                  double cy_raw,
                                                  double deg_raw,
                                                  double *deg_prev_out);

G_END_DECLS
