/*
 * Copyright (C) 2023 Canonical Ltd
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

#ifndef _CHANGES_H_
#define _CHANGES_H_

#include "ds_state.h"

// check for task changes in a Change three times per second
#define CHANGE_CHECK_INTERVAL 333

typedef struct {
  gchar *change_id;
  gint timeout_id;
  DsState *state;
  gboolean busy;
} SnapProgress;

void manage_snap_dbus(GDBusConnection *connection, DsState *state);

#endif // _CHANGES_H_
