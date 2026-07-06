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
 * cmocka unit tests for the MCP remote-edit discovery record
 * (src/control/remote_discovery.c): schema, atomic rename, permissions,
 * stale-record tolerance, and log redaction. No sockets, no darktable
 * init -- config_dir is an explicit scratch directory per test.
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../util/assert.h"

#include "control/remote_discovery.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* ---------------------------------------------------------------------- */
/* scratch directory helpers                                              */
/* ---------------------------------------------------------------------- */

static void _rmdir_recursive(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if(dir)
  {
    const gchar *name;
    while((name = g_dir_read_name(dir)))
    {
      gchar *child = g_build_filename(path, name, NULL);
      if(g_file_test(child, G_FILE_TEST_IS_DIR)) _rmdir_recursive(child);
      else g_remove(child);
      g_free(child);
    }
    g_dir_close(dir);
  }
  g_rmdir(path);
}

static int _setup_scratch_dir(void **state)
{
  gchar *dir = g_dir_make_tmp("dt_remote_discovery_test_XXXXXX", NULL);
  assert_non_null(dir);
  *state = dir;
  return 0;
}

static int _teardown_scratch_dir(void **state)
{
  gchar *dir = *state;
  _rmdir_recursive(dir);
  g_free(dir);
  return 0;
}

// Lists the plain filenames (not full paths) directly under
// `<config_dir>/mcp`, sorted, for assertions about exactly which files
// exist (e.g. no leftover temp sibling after a write).
static GPtrArray *_list_mcp_dir(const char *config_dir)
{
  gchar *mcp_dir = g_build_filename(config_dir, "mcp", NULL);
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  GDir *dir = g_dir_open(mcp_dir, 0, NULL);
  if(dir)
  {
    const gchar *name;
    while((name = g_dir_read_name(dir))) g_ptr_array_add(names, g_strdup(name));
    g_dir_close(dir);
  }
  g_free(mcp_dir);
  g_ptr_array_sort(names, (GCompareFunc)g_strcmp0);
  return names;
}

static gboolean _names_contains(GPtrArray *names, const char *name)
{
  for(guint i = 0; i < names->len; i++)
    if(g_strcmp0(g_ptr_array_index(names, i), name) == 0) return TRUE;
  return FALSE;
}

/* ---------------------------------------------------------------------- */
/* record schema                                                          */
/* ---------------------------------------------------------------------- */

static void test_write_produces_parseable_record_with_expected_schema(void **state)
{
  const char *config_dir = *state;

  char *path = dt_remote_discovery_write(config_dir, 43127, "sGVsbG8td29ybGQ", "5.7.0");
  assert_non_null(path);
  assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

  gchar *contents = NULL;
  gsize len = 0;
  assert_true(g_file_get_contents(path, &contents, &len, NULL));

  JsonParser *parser = json_parser_new();
  assert_true(json_parser_load_from_data(parser, contents, (gssize)len, NULL));
  JsonObject *obj = json_node_get_object(json_parser_get_root(parser));

  assert_true(json_object_has_member(obj, "protocol"));
  assert_string_equal(json_object_get_string_member(obj, "protocol"), "org.darktable.remote-edit");
  assert_int_equal((int)json_object_get_int_member(obj, "protocol_version"), 1);
  assert_string_equal(json_object_get_string_member(obj, "darktable_version"), "5.7.0");
  assert_int_equal((int)json_object_get_int_member(obj, "pid"), (int)getpid());
  assert_string_equal(json_object_get_string_member(obj, "host"), "127.0.0.1");
  assert_int_equal((int)json_object_get_int_member(obj, "port"), 43127);
  assert_string_equal(json_object_get_string_member(obj, "token"), "sGVsbG8td29ybGQ");
  assert_true(json_object_has_member(obj, "started_at"));
  assert_true(strlen(json_object_get_string_member(obj, "started_at")) > 0);

  g_object_unref(parser);
  g_free(contents);

  // filename matches the documented session-<pid>.json pattern
  gchar *expected_name = g_strdup_printf("session-%d.json", (int)getpid());
  gchar *basename = g_path_get_basename(path);
  assert_string_equal(basename, expected_name);
  g_free(basename);
  g_free(expected_name);

  dt_remote_discovery_remove(path);
  g_free(path);
}

/* ---------------------------------------------------------------------- */
/* atomic rename                                                           */
/* ---------------------------------------------------------------------- */

static void test_write_leaves_no_temp_sibling_behind(void **state)
{
  const char *config_dir = *state;

  char *path = dt_remote_discovery_write(config_dir, 1, "token", "5.7.0");
  assert_non_null(path);

  GPtrArray *names = _list_mcp_dir(config_dir);
  gchar *expected_name = g_strdup_printf("session-%d.json", (int)getpid());
  assert_int_equal((int)names->len, 1);
  assert_true(_names_contains(names, expected_name));

  g_free(expected_name);
  g_ptr_array_unref(names);
  dt_remote_discovery_remove(path);
  g_free(path);
}

static void test_write_creates_mcp_subdirectory(void **state)
{
  const char *config_dir = *state;
  gchar *mcp_dir = g_build_filename(config_dir, "mcp", NULL);
  assert_false(g_file_test(mcp_dir, G_FILE_TEST_EXISTS));

  char *path = dt_remote_discovery_write(config_dir, 1, "token", "5.7.0");
  assert_non_null(path);
  assert_true(g_file_test(mcp_dir, G_FILE_TEST_IS_DIR));

  g_free(mcp_dir);
  dt_remote_discovery_remove(path);
  g_free(path);
}

/* ---------------------------------------------------------------------- */
/* permissions                                                             */
/* ---------------------------------------------------------------------- */

static void test_write_sets_user_only_permissions(void **state)
{
  const char *config_dir = *state;

  char *path = dt_remote_discovery_write(config_dir, 1, "token", "5.7.0");
  assert_non_null(path);

  struct stat st;
  assert_int_equal(g_stat(path, &st), 0);
  const mode_t perms = st.st_mode & 0777;
  assert_int_equal((int)perms, 0600);

  dt_remote_discovery_remove(path);
  g_free(path);
}

/* ---------------------------------------------------------------------- */
/* stale-record tolerance                                                  */
/* ---------------------------------------------------------------------- */

static void test_write_tolerates_unrelated_stale_record(void **state)
{
  const char *config_dir = *state;
  gchar *mcp_dir = g_build_filename(config_dir, "mcp", NULL);
  assert_int_equal(g_mkdir_with_parents(mcp_dir, 0700), 0);

  // a record left behind by some other (long-dead) pid, with garbage
  // content -- our writer must not choke on it, and must not touch it.
  gchar *stale_path = g_build_filename(mcp_dir, "session-999999.json", NULL);
  assert_true(g_file_set_contents(stale_path, "not even json{{{", -1, NULL));

  char *path = dt_remote_discovery_write(config_dir, 1, "token", "5.7.0");
  assert_non_null(path);

  assert_true(g_file_test(stale_path, G_FILE_TEST_EXISTS));
  gchar *stale_contents = NULL;
  assert_true(g_file_get_contents(stale_path, &stale_contents, NULL, NULL));
  assert_string_equal(stale_contents, "not even json{{{");

  g_free(stale_contents);
  g_free(stale_path);
  g_free(mcp_dir);
  dt_remote_discovery_remove(path);
  g_free(path);
}

/* ---------------------------------------------------------------------- */
/* removal / shutdown                                                     */
/* ---------------------------------------------------------------------- */

static void test_remove_deletes_the_record(void **state)
{
  const char *config_dir = *state;
  char *path = dt_remote_discovery_write(config_dir, 1, "token", "5.7.0");
  assert_non_null(path);
  assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

  dt_remote_discovery_remove(path);
  assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
  g_free(path);
}

static void test_remove_is_null_and_missing_file_safe(void **state)
{
  (void)state;
  dt_remote_discovery_remove(NULL);            // must not crash
  dt_remote_discovery_remove("/no/such/file");  // must not crash
}

/* ---------------------------------------------------------------------- */
/* log redaction                                                           */
/* ---------------------------------------------------------------------- */

static void test_describe_for_log_never_contains_the_token(void **state)
{
  (void)state;
  const char *secret = "sUpEr-SeCrEt-t0ken-value-that-must-never-be-logged";
  char *desc = dt_remote_discovery_describe_for_log("/tmp/x/mcp/session-42.json", 12345, secret);
  assert_non_null(desc);
  assert_null(strstr(desc, secret));
  // sanity: the description is still informative about non-secret fields
  assert_non_null(strstr(desc, "12345"));
  g_free(desc);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_write_produces_parseable_record_with_expected_schema,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test_setup_teardown(test_write_leaves_no_temp_sibling_behind,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test_setup_teardown(test_write_creates_mcp_subdirectory,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test_setup_teardown(test_write_sets_user_only_permissions,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test_setup_teardown(test_write_tolerates_unrelated_stale_record,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test_setup_teardown(test_remove_deletes_the_record,
                                    _setup_scratch_dir, _teardown_scratch_dir),
    cmocka_unit_test(test_remove_is_null_and_missing_file_safe),
    cmocka_unit_test(test_describe_for_log_never_contains_the_token),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
