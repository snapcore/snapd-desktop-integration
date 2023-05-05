/*
 * Copyright (C) 2020-2022 Canonical Ltd
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

#ifndef __DS_STATE_H__
#define __DS_STATE_H__

#include "io.snapcraft.SnapChanges.h"
#include "io.snapcraft.SnapDesktopIntegration.h"
#include <gtk/gtk.h>
#include <libnotify/notify.h>
#include <snapd-glib/snapd-glib.h>

#define ICON_SIZE 48

typedef struct {
  SnapDesktopIntegration *skeleton;
  SnapChanges *snap_dbus_proxy;

  GtkSettings *settings;
  SnapdClient *client;
  GtkApplication *app;

  /* Timer to delay checking after theme changes */
  guint check_delay_timer_id;

  /* Name of current themes and their status in snapd. */
  gchar *gtk_theme_name;
  SnapdThemeStatus gtk_theme_status;
  gchar *icon_theme_name;
  SnapdThemeStatus icon_theme_status;
  gchar *cursor_theme_name;
  SnapdThemeStatus cursor_theme_status;
  gchar *sound_theme_name;
  SnapdThemeStatus sound_theme_status;

  /* The desktop notifications */
  NotifyNotification *install_notification;
  NotifyNotification *progress_notification;

  /* The list of current refresh popups */
  GList *refreshing_list;
} DsState;

#endif // __DS_STATE_H__
