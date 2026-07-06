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

#include "control/remote_discovery.h"

#include "common/darktable.h"

#include <errno.h>
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char *dt_remote_discovery_describe_for_log(const char *path, int port,
                                           const char *token_b64)
{
  (void)token_b64;  // intentionally unused: this string must never contain it
  return g_strdup_printf("[remote-edit] discovery record path='%s' port=%d",
                        path ? path : "(null)", port);
}

static gboolean _write_all_and_close(int fd, const char *data, gsize len)
{
  gsize written = 0;
  gboolean ok = TRUE;
  while(written < len)
  {
    const ssize_t n = write(fd, data + written, len - written);
    if(n > 0) { written += (gsize)n; continue; }
    if(n < 0 && errno == EINTR) continue;
    ok = FALSE;
    break;
  }
  if(ok) ok = (fsync(fd) == 0);
  if(close(fd) != 0) ok = FALSE;
  return ok;
}

char *dt_remote_discovery_write(const char *config_dir, int port,
                                const char *token_b64,
                                const char *darktable_version)
{
  g_return_val_if_fail(config_dir != NULL, NULL);
  g_return_val_if_fail(token_b64 != NULL, NULL);

  gchar *mcp_dir = g_build_filename(config_dir, "mcp", NULL);
  if(g_mkdir_with_parents(mcp_dir, 0700) != 0)
  {
    char *desc = dt_remote_discovery_describe_for_log(mcp_dir, port, token_b64);
    dt_print(DT_DEBUG_CONTROL, "[remote-edit] failed to create discovery directory: %s", desc);
    g_free(desc);
    g_free(mcp_dir);
    return NULL;
  }

  const pid_t pid = getpid();
  gchar *final_name = g_strdup_printf("session-%d.json", (int)pid);
  gchar *final_path = g_build_filename(mcp_dir, final_name, NULL);
  gchar *tmp_path = g_strdup_printf("%s.tmp-%d", final_path, (int)pid);
  g_free(final_name);

  // started_at as UTC ISO-8601 -- built with strftime rather than
  // g_date_time_format_iso8601() (GLib >= 2.62) to stay inside
  // darktable's documented GLib 2.56 floor.
  time_t now = time(NULL);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char started_at[32];
  strftime(started_at, sizeof(started_at), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "protocol");
  json_builder_add_string_value(b, "org.darktable.remote-edit");
  json_builder_set_member_name(b, "protocol_version");
  json_builder_add_int_value(b, 1);
  json_builder_set_member_name(b, "darktable_version");
  json_builder_add_string_value(b, darktable_version ? darktable_version : "");
  json_builder_set_member_name(b, "pid");
  json_builder_add_int_value(b, (gint64)pid);
  json_builder_set_member_name(b, "host");
  json_builder_add_string_value(b, "127.0.0.1");
  json_builder_set_member_name(b, "port");
  json_builder_add_int_value(b, port);
  json_builder_set_member_name(b, "token");
  json_builder_add_string_value(b, token_b64);
  json_builder_set_member_name(b, "started_at");
  json_builder_add_string_value(b, started_at);
  json_builder_end_object(b);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(b));
  gsize out_len = 0;
  gchar *out_data = json_generator_to_data(gen, &out_len);
  g_object_unref(gen);
  g_object_unref(b);

  gboolean ok = FALSE;
  // O_EXCL: refuse to reuse a leftover temp file from a previous crash of
  // this exact pid (astronomically unlikely, but cheap to rule out) --
  // explicit mode 0600 at creation time means there is no window, even
  // under a permissive umask, where the temp file is anything but
  // user-only, before the chmod below runs as defense in depth.
  int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  if(fd < 0 && errno == EEXIST)
  {
    g_remove(tmp_path);
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
  }
  if(fd >= 0)
  {
    ok = _write_all_and_close(fd, out_data, out_len);
    if(ok)
    {
      // explicit chmod before the token is ever visible at the final
      // path: belt-and-braces alongside the explicit creation mode
      // above (e.g. in case the platform's open() mode argument is
      // masked by something other than a plain umask).
      g_chmod(tmp_path, S_IRUSR | S_IWUSR);
      ok = (g_rename(tmp_path, final_path) == 0);
    }
    if(!ok) g_remove(tmp_path);
  }

  g_free(out_data);

  char *result = NULL;
  if(ok)
  {
    result = final_path;  // ownership transferred to caller
  }
  else
  {
    char *desc = dt_remote_discovery_describe_for_log(final_path, port, token_b64);
    dt_print(DT_DEBUG_CONTROL, "[remote-edit] failed to write discovery record: %s", desc);
    g_free(desc);
    g_free(final_path);
  }

  g_free(tmp_path);
  g_free(mcp_dir);
  return result;
}

void dt_remote_discovery_remove(const char *path)
{
  if(!path) return;
  g_remove(path);  // tolerates a missing file (already removed/stale); no error to report
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
