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

#include "control/remote_transform.h"

#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"

#include <math.h>

static void _sizes(float *pw, float *ph, float *iw, float *ih)
{
  dt_masks_get_image_size(pw, ph, iw, ih);
}

gboolean dt_remote_transform_ensure_fresh(dt_develop_t *dev,
                                          dt_remote_error_t **error)
{
  if(!dev || !dev->preview_pipe)
  {
    if(error)
      *error = dt_remote_error_new(DT_REMOTE_ERR_INTERNAL, "no preview pipe");
    return FALSE;
  }
  if(dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
    return TRUE;

  int nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
  if(nloop <= 0)
    nloop = 2000;

  // Enqueue one reprocess. This is a no-op when gui_attached is FALSE; a
  // worker otherwise updates the pipe status independently of this poll.
  dt_dev_process_preview(dev);

  for(int n = 0; n < nloop; n++)
  {
    const dt_dev_pixelpipe_status_t status = dev->preview_pipe->status;
    if(status == DT_DEV_PIXELPIPE_VALID)
      return TRUE;
    if(status == DT_DEV_PIXELPIPE_INVALID)
      goto not_ready;
    dt_iop_nap(5000);
  }

  // The pipe may become valid during the final wait.
  if(dev->preview_pipe->status == DT_DEV_PIXELPIPE_VALID)
    return TRUE;

not_ready:
  if(error)
    *error = dt_remote_error_new(DT_REMOTE_ERR_PIPE_NOT_READY,
                                 "preview pipe not ready; retry");
  return FALSE;
}

void dt_remote_transform_preview_to_raw_point(dt_develop_t *dev,
                                              double px,
                                              double py,
                                              double *rx,
                                              double *ry)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  float points[2] = { (float)(px * pw), (float)(py * ph) };
  dt_dev_distort_backtransform(dev, points, 1);
  *rx = points[0] / iw;
  *ry = points[1] / ih;
}

void dt_remote_transform_raw_to_preview_point(dt_develop_t *dev,
                                              double rx,
                                              double ry,
                                              double *px,
                                              double *py)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  float points[2] = { (float)(rx * iw), (float)(ry * ih) };
  dt_dev_distort_transform(dev, points, 1);
  *px = points[0] / pw;
  *py = points[1] / ph;
}

static gboolean _probe_raw_distance(dt_develop_t *dev,
                                    double cx_prev,
                                    double cy_prev,
                                    double r_prev,
                                    double *mean_out,
                                    gboolean *exact_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double denominator = fmin(iw, ih);

  float points[10] = {
    (float)(cx_prev * pw),            (float)(cy_prev * ph),
    (float)(cx_prev * pw),            (float)((cy_prev - r_prev) * ph),
    (float)(cx_prev * pw),            (float)((cy_prev + r_prev) * ph),
    (float)((cx_prev - r_prev) * pw), (float)(cy_prev * ph),
    (float)((cx_prev + r_prev) * pw), (float)(cy_prev * ph),
  };
  if(!dt_dev_distort_backtransform(dev, points, 5))
    return FALSE;

  const double center_x = points[0];
  const double center_y = points[1];
  double sum = 0.0;
  double minimum = INFINITY;
  double maximum = 0.0;
  for(int k = 0; k < 4; k++)
  {
    const double dx = points[2 + k * 2] - center_x;
    const double dy = points[3 + k * 2] - center_y;
    const double distance = hypot(dx, dy);
    sum += distance;
    minimum = fmin(minimum, distance);
    maximum = fmax(maximum, distance);
  }

  const double mean = sum / 4.0;
  *mean_out = denominator > 0.0 ? mean / denominator : 0.0;
  if(exact_out)
    *exact_out = mean > 0.0
                   ? (maximum - minimum) / mean <= DT_REMOTE_TRANSFORM_SPREAD
                   : TRUE;
  return TRUE;
}

gboolean dt_remote_transform_preview_to_raw_size(dt_develop_t *dev,
                                                 double cx_prev,
                                                 double cy_prev,
                                                 double r_prev,
                                                 double *r_raw_out,
                                                 gboolean *exact_out)
{
  return _probe_raw_distance(dev, cx_prev, cy_prev, r_prev,
                             r_raw_out, exact_out);
}

gboolean dt_remote_transform_raw_to_preview_size(dt_develop_t *dev,
                                                 double cx_raw,
                                                 double cy_raw,
                                                 double r_raw,
                                                 double *r_prev_out,
                                                 gboolean *exact_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double raw_radius = r_raw * fmin(iw, ih);

  float points[10] = {
    (float)(cx_raw * iw),              (float)(cy_raw * ih),
    (float)(cx_raw * iw),              (float)(cy_raw * ih - raw_radius),
    (float)(cx_raw * iw),              (float)(cy_raw * ih + raw_radius),
    (float)(cx_raw * iw - raw_radius), (float)(cy_raw * ih),
    (float)(cx_raw * iw + raw_radius), (float)(cy_raw * ih),
  };
  if(!dt_dev_distort_transform(dev, points, 5))
    return FALSE;

  const double center_x = points[0];
  const double center_y = points[1];
  double sum = 0.0;
  double minimum = INFINITY;
  double maximum = 0.0;
  for(int k = 0; k < 4; k++)
  {
    const double dx = (points[2 + k * 2] - center_x) / pw;
    const double dy = (points[3 + k * 2] - center_y) / ph;
    const double distance = hypot(dx, dy);
    sum += distance;
    minimum = fmin(minimum, distance);
    maximum = fmax(maximum, distance);
  }

  const double mean = sum / 4.0;
  *r_prev_out = mean;
  if(exact_out)
    *exact_out = mean > 0.0
                   ? (maximum - minimum) / mean <= DT_REMOTE_TRANSFORM_SPREAD
                   : TRUE;
  return TRUE;
}

static gboolean _probe_angle(dt_develop_t *dev,
                             gboolean forward,
                             double cx,
                             double cy,
                             double deg_in,
                             double *deg_out)
{
  float pw, ph, iw, ih;
  _sizes(&pw, &ph, &iw, &ih);
  const double source_width = forward ? iw : pw;
  const double source_height = forward ? ih : ph;
  const double radians = deg_in * M_PI / 180.0;
  const double arm =
    DT_REMOTE_TRANSFORM_ANGLE_ARM * fmin(source_width, source_height);

  float points[4] = {
    (float)(cx * source_width),
    (float)(cy * source_height),
    (float)(cx * source_width + arm * cos(radians)),
    (float)(cy * source_height - arm * sin(radians)),
  };
  const gboolean success =
    forward ? dt_dev_distort_transform(dev, points, 2)
            : dt_dev_distort_backtransform(dev, points, 2);
  if(!success)
    return FALSE;

  const double dx = points[2] - points[0];
  const double dy = points[3] - points[1];
  *deg_out = atan2(-dy, dx) * 180.0 / M_PI;
  return TRUE;
}

gboolean dt_remote_transform_preview_to_raw_angle(dt_develop_t *dev,
                                                  double cx_prev,
                                                  double cy_prev,
                                                  double deg_prev,
                                                  double *deg_raw_out)
{
  return _probe_angle(dev, FALSE, cx_prev, cy_prev, deg_prev, deg_raw_out);
}

gboolean dt_remote_transform_raw_to_preview_angle(dt_develop_t *dev,
                                                  double cx_raw,
                                                  double cy_raw,
                                                  double deg_raw,
                                                  double *deg_prev_out)
{
  return _probe_angle(dev, TRUE, cx_raw, cy_raw, deg_raw, deg_prev_out);
}
