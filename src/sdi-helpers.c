/*
 * Copyright (C) 2024 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sdi-helpers.h"

GAppInfo *sdi_get_desktop_file_from_snap(SnapdSnap *snap) {
  GPtrArray *apps = snapd_snap_get_apps(snap);
  if ((apps == NULL) || (apps->len == 0)) {
    return NULL;
  }

  if (apps->len == 1) {
    const gchar *desktop_file = snapd_app_get_desktop_file(apps->pdata[0]);
    if (desktop_file == NULL)
      return NULL;
    return G_APP_INFO(g_desktop_app_info_new_from_filename(desktop_file));
  }

  const gchar *name = snapd_snap_get_name(snap);
  // get the entry that has the same app name than the snap
  for (guint i = 0; i < apps->len; i++) {
    SnapdApp *app = apps->pdata[i];
    if (g_str_equal(name, snapd_app_get_name(app))) {
      const gchar *desktop_file = snapd_app_get_desktop_file(app);
      if (desktop_file == NULL)
        return NULL;
      return G_APP_INFO(g_desktop_app_info_new_from_filename(desktop_file));
    }
  }
  // if it doesn't exist, get the first entry with an icon
  for (guint i = 0; i < apps->len; i++) {
    SnapdApp *app = apps->pdata[i];
    const gchar *desktop_file = snapd_app_get_desktop_file(app);
    if (desktop_file == NULL)
      continue;
    g_autoptr(GAppInfo) app_info =
        G_APP_INFO(g_desktop_app_info_new_from_filename(desktop_file));
    if (app_info != NULL) {
      GIcon *icon = g_app_info_get_icon(app_info);
      if (icon != NULL) {
        return g_steal_pointer(&app_info);
      }
    }
  }
  return NULL;
}

GPtrArray *sdi_get_desktop_filenames_for_snap(const gchar *snap_name) {
  g_autoptr(GDir) desktop_folder =
      g_dir_open("/var/lib/snapd/desktop/applications", 0, NULL);
  if (desktop_folder == NULL) {
    return NULL;
  }
  g_autofree gchar *prefix = g_strdup_printf("%s_", snap_name);
  const gchar *filename;
  GPtrArray *desktop_files = g_ptr_array_new_with_free_func(g_free);
  while ((filename = g_dir_read_name(desktop_folder)) != NULL) {
    if (!g_str_has_prefix(filename, prefix)) {
      continue;
    }
    if (!g_str_has_suffix(filename, ".desktop")) {
      continue;
    }
    g_ptr_array_add(desktop_files, g_strdup(filename));
  }
  return desktop_files;
}
