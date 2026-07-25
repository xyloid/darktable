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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "common/darktable.h"
#include "control/conf.h"
#include "control/remote_edit.h"
#include "control/remote_transform.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef DT_TEST_MODULEDIR
#error "DT_TEST_MODULEDIR must be defined by the build (see CMakeLists.txt)"
#endif

static char *s_confdir = NULL;

typedef struct transform_fixture_t
{
  dt_dev_pixelpipe_t *preview_pipe;
  int processed_width;
  int processed_height;
  int iwidth;
  int iheight;
  float iscale;
  dt_dev_pixelpipe_status_t status;
  gboolean gui_attached;
  int synchronization_timeout;
  dt_iop_module_t anisotropic_module;
  dt_dev_pixelpipe_iop_t anisotropic_piece;
  gboolean anisotropic_installed;
} transform_fixture_t;

static gboolean _anisotropic_forward(dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *piece,
                                     float *points,
                                     const size_t points_count)
{
  (void)self;
  (void)piece;
  for(size_t k = 0; k < points_count; k++)
    points[2 * k] *= 2.0f;
  return TRUE;
}

static gboolean _anisotropic_back(dt_iop_module_t *self,
                                  dt_dev_pixelpipe_iop_t *piece,
                                  float *points,
                                  const size_t points_count)
{
  (void)self;
  (void)piece;
  for(size_t k = 0; k < points_count; k++)
    points[2 * k] *= 0.5f;
  return TRUE;
}

static void _install_anisotropic_transform(transform_fixture_t *fixture)
{
  fixture->anisotropic_module.dev = darktable.develop;
  fixture->anisotropic_module.distort_transform = _anisotropic_forward;
  fixture->anisotropic_module.distort_backtransform = _anisotropic_back;
  fixture->anisotropic_piece.module = &fixture->anisotropic_module;
  fixture->anisotropic_piece.pipe = fixture->preview_pipe;
  fixture->anisotropic_piece.data = &fixture->anisotropic_piece;
  fixture->anisotropic_piece.enabled = TRUE;
  fixture->preview_pipe->iop =
    g_list_prepend(fixture->preview_pipe->iop, &fixture->anisotropic_module);
  fixture->preview_pipe->nodes =
    g_list_prepend(fixture->preview_pipe->nodes, &fixture->anisotropic_piece);
  fixture->anisotropic_installed = TRUE;
}

static int harness_group_setup(void **state)
{
  (void)state;
  GError *gerror = NULL;
  s_confdir = g_dir_make_tmp("test_remote_transform-XXXXXX", &gerror);
  if(!s_confdir)
  {
    fprintf(stderr, "test_remote_transform: failed to create scratch config dir: %s\n",
            gerror->message);
    g_error_free(gerror);
    return -1;
  }

  char *argv_override[] = {
    "test_remote_transform",
    "--configdir", s_confdir,
    "--library", ":memory:",
    "--moduledir", DT_TEST_MODULEDIR,
    "--conf", "write_sidecar_files=never",
    NULL
  };
  int argc_override = G_N_ELEMENTS(argv_override) - 1;
  return dt_init(argc_override, argv_override, FALSE, FALSE, NULL) ? -1 : 0;
}

static int harness_group_teardown(void **state)
{
  (void)state;
  dt_cleanup();
  if(s_confdir)
  {
    gchar *cmd = g_strdup_printf("rm -rf '%s'", s_confdir);
    if(system(cmd) != 0)
      fprintf(stderr, "test_remote_transform: failed to remove scratch config dir %s\n",
              s_confdir);
    g_free(cmd);
    g_clear_pointer(&s_confdir, g_free);
  }
  return 0;
}

static int transform_test_setup(void **state)
{
  assert_non_null(darktable.develop);
  assert_non_null(darktable.develop->preview_pipe);

  transform_fixture_t *fixture = g_new0(transform_fixture_t, 1);
  fixture->preview_pipe = darktable.develop->preview_pipe;
  fixture->processed_width = fixture->preview_pipe->processed_width;
  fixture->processed_height = fixture->preview_pipe->processed_height;
  fixture->iwidth = fixture->preview_pipe->iwidth;
  fixture->iheight = fixture->preview_pipe->iheight;
  fixture->iscale = fixture->preview_pipe->iscale;
  fixture->status = fixture->preview_pipe->status;
  fixture->gui_attached = darktable.develop->gui_attached;
  fixture->synchronization_timeout =
    dt_conf_get_int("pixelpipe_synchronization_timeout");

  // dt_init leaves these dimensions at zero. A square identity fixture
  // makes normalized preview and raw coordinates deterministic.
  fixture->preview_pipe->processed_width = 1000;
  fixture->preview_pipe->processed_height = 1000;
  fixture->preview_pipe->iwidth = 1000;
  fixture->preview_pipe->iheight = 1000;
  fixture->preview_pipe->iscale = 1.0f;

  *state = fixture;
  return 0;
}

static int transform_test_teardown(void **state)
{
  transform_fixture_t *fixture = *state;
  if(!fixture) return 0;

  // The null-pipe test deliberately clears this pointer. Restore it before
  // restoring the pipe fields and before the group-level dt_cleanup().
  darktable.develop->preview_pipe = fixture->preview_pipe;
  if(fixture->anisotropic_installed)
  {
    fixture->preview_pipe->iop =
      g_list_remove(fixture->preview_pipe->iop, &fixture->anisotropic_module);
    fixture->preview_pipe->nodes =
      g_list_remove(fixture->preview_pipe->nodes, &fixture->anisotropic_piece);
  }
  fixture->preview_pipe->processed_width = fixture->processed_width;
  fixture->preview_pipe->processed_height = fixture->processed_height;
  fixture->preview_pipe->iwidth = fixture->iwidth;
  fixture->preview_pipe->iheight = fixture->iheight;
  fixture->preview_pipe->iscale = fixture->iscale;
  fixture->preview_pipe->status = fixture->status;
  darktable.develop->gui_attached = fixture->gui_attached;
  dt_conf_set_int("pixelpipe_synchronization_timeout",
                  fixture->synchronization_timeout);
  g_free(fixture);
  *state = NULL;
  return 0;
}

static void test_error_constructor_is_exported(void **state)
{
  (void)state;
  dt_remote_error_t *error =
    dt_remote_error_new(DT_REMOTE_ERR_NOT_FOUND, "unknown mask shape %d", 17);
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_NOT_FOUND);
  assert_string_equal(error->message, "unknown mask shape 17");
  assert_null(error->details_json);
  dt_remote_error_free(error);
}

static void test_preview_to_raw_point_identity(void **state)
{
  (void)state;
  double rx = 0.0;
  double ry = 0.0;
  dt_remote_transform_preview_to_raw_point(darktable.develop, 0.25, 0.75, &rx, &ry);
  assert_float_equal(rx, 0.25, 1e-4);
  assert_float_equal(ry, 0.75, 1e-4);
}

static void test_raw_to_preview_point_identity(void **state)
{
  (void)state;
  double px = 0.0;
  double py = 0.0;
  dt_remote_transform_raw_to_preview_point(darktable.develop, 0.25, 0.75, &px, &py);
  assert_float_equal(px, 0.25, 1e-4);
  assert_float_equal(py, 0.75, 1e-4);
}

static void test_preview_to_raw_size_identity_is_exact(void **state)
{
  (void)state;
  double r_raw = 0.0;
  gboolean exact = FALSE;
  assert_true(dt_remote_transform_preview_to_raw_size(
    darktable.develop, 0.5, 0.5, 0.1, &r_raw, &exact));
  assert_float_equal(r_raw, 0.1, 1e-4);
  assert_true(exact);
}

static void test_raw_to_preview_size_identity_is_exact(void **state)
{
  (void)state;
  double r_preview = 0.0;
  gboolean exact = FALSE;
  assert_true(dt_remote_transform_raw_to_preview_size(
    darktable.develop, 0.5, 0.5, 0.1, &r_preview, &exact));
  assert_float_equal(r_preview, 0.1, 1e-4);
  assert_true(exact);
}

static void test_size_non_square_identity_roundtrips_exactly(void **state)
{
  transform_fixture_t *fixture = *state;
  fixture->preview_pipe->processed_width = 1200;
  fixture->preview_pipe->processed_height = 800;
  fixture->preview_pipe->iwidth = 1200;
  fixture->preview_pipe->iheight = 800;

  double r_raw = 0.0;
  gboolean exact = FALSE;
  assert_true(dt_remote_transform_preview_to_raw_size(
    darktable.develop, 0.5, 0.5, 0.1, &r_raw, &exact));
  assert_float_equal(r_raw, 0.1, 1e-4);
  assert_true(exact);

  double r_preview = 0.0;
  exact = FALSE;
  assert_true(dt_remote_transform_raw_to_preview_size(
    darktable.develop, 0.5, 0.5, r_raw, &r_preview, &exact));
  assert_float_equal(r_preview, 0.1, 1e-4);
  assert_true(exact);
}

static void test_preview_to_raw_size_marks_anisotropy_approximate(void **state)
{
  transform_fixture_t *fixture = *state;
  _install_anisotropic_transform(fixture);

  double r_raw = 0.0;
  gboolean exact = TRUE;
  assert_true(dt_remote_transform_preview_to_raw_size(
    darktable.develop, 0.5, 0.5, 0.1, &r_raw, &exact));
  assert_float_equal(r_raw, 0.075, 1e-4);
  assert_false(exact);
}

static void test_raw_to_preview_size_marks_anisotropy_approximate(void **state)
{
  transform_fixture_t *fixture = *state;
  _install_anisotropic_transform(fixture);

  double r_preview = 0.0;
  gboolean exact = TRUE;
  assert_true(dt_remote_transform_raw_to_preview_size(
    darktable.develop, 0.5, 0.5, 0.1, &r_preview, &exact));
  assert_float_equal(r_preview, 0.15, 1e-4);
  assert_false(exact);
}

static void test_preview_to_raw_angle_identity(void **state)
{
  (void)state;
  double deg_raw = 0.0;
  assert_true(dt_remote_transform_preview_to_raw_angle(
    darktable.develop, 0.5, 0.5, 37.5, &deg_raw));
  assert_float_equal(deg_raw, 37.5, 1e-3);
}

static void test_raw_to_preview_angle_identity(void **state)
{
  (void)state;
  double deg_preview = 0.0;
  assert_true(dt_remote_transform_raw_to_preview_angle(
    darktable.develop, 0.5, 0.5, -42.0, &deg_preview));
  assert_float_equal(deg_preview, -42.0, 1e-3);
}

static void test_ensure_fresh_valid_passes(void **state)
{
  transform_fixture_t *fixture = *state;
  fixture->preview_pipe->status = DT_DEV_PIXELPIPE_VALID;

  dt_remote_error_t *error = NULL;
  assert_true(dt_remote_transform_ensure_fresh(darktable.develop, &error));
  assert_null(error);
}

static void test_ensure_fresh_dirty_times_out_to_retry_later(void **state)
{
  transform_fixture_t *fixture = *state;
  dt_conf_set_int("pixelpipe_synchronization_timeout", 1);
  darktable.develop->gui_attached = FALSE;
  fixture->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_transform_ensure_fresh(darktable.develop, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_PIPE_NOT_READY);
  dt_remote_error_free(error);
}

static void test_ensure_fresh_invalid_stops_nonpositive_fallback(void **state)
{
  transform_fixture_t *fixture = *state;
  dt_conf_set_int("pixelpipe_synchronization_timeout", 0);
  darktable.develop->gui_attached = FALSE;
  fixture->preview_pipe->status = DT_DEV_PIXELPIPE_INVALID;

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_transform_ensure_fresh(darktable.develop, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_PIPE_NOT_READY);
  dt_remote_error_free(error);
}

static void test_ensure_fresh_null_pipe_returns_internal_error(void **state)
{
  (void)state;
  darktable.develop->preview_pipe = NULL;

  dt_remote_error_t *error = NULL;
  assert_false(dt_remote_transform_ensure_fresh(darktable.develop, &error));
  assert_non_null(error);
  assert_int_equal(error->code, DT_REMOTE_ERR_INTERNAL);
  assert_string_equal(error->message, "no preview pipe");
  dt_remote_error_free(error);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_error_constructor_is_exported,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_preview_to_raw_point_identity,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_raw_to_preview_point_identity,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_preview_to_raw_size_identity_is_exact,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_raw_to_preview_size_identity_is_exact,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_size_non_square_identity_roundtrips_exactly,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_preview_to_raw_size_marks_anisotropy_approximate,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_raw_to_preview_size_marks_anisotropy_approximate,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_preview_to_raw_angle_identity,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_raw_to_preview_angle_identity,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_ensure_fresh_valid_passes,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_ensure_fresh_dirty_times_out_to_retry_later,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_ensure_fresh_invalid_stops_nonpositive_fallback,
                                    transform_test_setup, transform_test_teardown),
    cmocka_unit_test_setup_teardown(test_ensure_fresh_null_pipe_returns_internal_error,
                                    transform_test_setup, transform_test_teardown),
  };
  return cmocka_run_group_tests(tests, harness_group_setup, harness_group_teardown);
}
