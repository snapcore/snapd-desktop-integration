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

#pragma once

#include "io.snapcraft.SnapDesktopIntegration.h"
#include "sdi-theme-monitor.h"
#include <gtk/gtk.h>

#define ICON_SIZE 48

typedef struct {
  SnapDesktopIntegration *skeleton;

  SnapdClient *client;
  GtkApplication *app;

  SdiThemeMonitor *theme_monitor;

  /* The list of current refresh popups */
  GList *refreshing_list;
} DsState;
