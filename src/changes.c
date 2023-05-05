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

#include "changes.h"
#include "io.snapcraft.SnapChanges.h"
#include "refresh_status.h"

static void destroy_snap_progress(SnapProgress *data) {
  g_free(data->change_id);
  if (data->timeout_id != 0) {
    g_source_remove(data->timeout_id);
    data->timeout_id = 0;
  }
  g_free(data);
}

static void init_all_status(DsState *ds_state) {
  GList *list = ds_state->refreshing_list;
  for (; list != NULL; list = list->next) {
    RefreshState *state = (RefreshState *)list->data;
    state->ntasks = 0;
    state->doneTasks = 0;
    state->minId = G_MAXUINT;
    state->updated = FALSE;
  }
}

static void update_task(GVariant *task_value, SnapProgress *data) {
  GVariantIter entry_iter;
  GVariant *entry, *entry_key, *entry_container, *entry_value;
  g_autofree const gchar *instance_name = NULL;
  g_autofree const gchar *summary = NULL;
  g_autofree const gchar *status = NULL;
  guint task_id = G_MAXUINT;
  RefreshState *refresh_state;

  // process task
  g_variant_iter_init(&entry_iter, task_value);
  task_id = G_MAXUINT;
  while ((entry = g_variant_iter_next_value(&entry_iter)) != NULL) {
    entry_key = g_variant_get_child_value(entry, 0);
    entry_container = g_variant_get_child_value(entry, 1);
    entry_value = g_variant_get_child_value(entry_container, 0);
    if ((entry_key != NULL) && (entry_value != NULL)) {
      if (0 == g_strcmp0(g_variant_get_string(entry_key, NULL), "ID")) {
        task_id = atoi(g_variant_get_string(entry_value, NULL));
      } else if (0 ==
                 g_strcmp0(g_variant_get_string(entry_key, NULL), "Status")) {
        status = g_strdup(g_variant_get_string(entry_value, NULL));
      } else if (0 ==
                 g_strcmp0(g_variant_get_string(entry_key, NULL), "SnapName")) {
        instance_name = g_strdup(g_variant_get_string(entry_value, NULL));
      } else if (0 ==
                 g_strcmp0(g_variant_get_string(entry_key, NULL), "Summary")) {
        summary = g_strdup(g_variant_get_string(entry_value, NULL));
      }
    }
    g_variant_unref(entry_key);
    g_variant_unref(entry_container);
    g_variant_unref(entry_value);
  }

  if ((instance_name == NULL) || (instance_name[0] == 0)) {
    return;
  }

  refresh_state = ds_state_find_application(data->state, instance_name);
  if (refresh_state == NULL) {
    handle_application_is_being_refreshed((gchar *)instance_name, "", NULL,
                                          data->state);
    refresh_state = ds_state_find_application(data->state, instance_name);
  }
  refresh_state->updated = TRUE;
  refresh_state->ntasks++;
  if ((0 != g_strcmp0(status, "Doing")) && (0 != g_strcmp0(status, "Do"))) {
    refresh_state->doneTasks++;
  }
  if ((task_id < refresh_state->minId) && (summary != NULL)) {
    refresh_state->minId = task_id;
    g_free(refresh_state->summary);
    refresh_state->summary = g_strdup(summary);
  }
}

static gboolean update_refresh_notifications(SnapProgress *data) {
  GList *list = data->state->refreshing_list;
  gchar *barText;
  gboolean allDone = TRUE;
  for (; list != NULL; list = list->next) {
    RefreshState *state = (RefreshState *)list->data;
    if (state->updated) {
      if (state->ntasks == state->doneTasks) {
        handle_close_application_window(state->appName->str, NULL, data->state);
        return TRUE;
      } else {
        allDone = FALSE;
        state->pulsed = FALSE;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progressBar),
                                      ((gfloat)state->doneTasks) /
                                          ((gfloat)state->ntasks));
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progressBar),
                                       TRUE);
        barText = g_strdup_printf("%s (%d/%d)", state->summary,
                                  state->doneTasks, state->ntasks);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progressBar),
                                  barText);
        g_free(barText);
        g_free(state->summary);
        state->summary = NULL;
      }
    }
  }
  if (allDone) {
    destroy_snap_progress(data);
  }
  return FALSE;
}

static void receive_tasks(SnapChanges *proxy, GAsyncResult *res,
                          SnapProgress *data) {
  g_autoptr(GVariant) tasks = NULL;
  GVariant *task, *task_value;
  GVariantIter task_iter;

  data->busy = FALSE;
  if (!snap_changes_call_get_tasks_finish(proxy, &tasks, res, NULL)) {
    return;
  }

  init_all_status(data->state);

  g_variant_iter_init(&task_iter, tasks);
  while ((task = g_variant_iter_next_value(&task_iter)) != NULL) {
    task_value = g_variant_get_child_value(task, 1);
    if (task_value == NULL) {
      continue;
    }
    update_task(task_value, data);
    g_variant_unref(task_value);
    g_variant_unref(task);
  }
  while (update_refresh_notifications(data)) {
  }
}

static gboolean launch_progress_cb(SnapProgress *data) {
  g_autoptr(GVariantType) extra_data_type = g_variant_type_new("{sv}");
  if (data->busy) {
    return TRUE;
  }
  data->busy = TRUE;
  snap_changes_call_get_tasks(data->state->snap_dbus_proxy, data->change_id,
                              g_variant_new_array(extra_data_type, NULL, 0),
                              NULL, (GAsyncReadyCallback)receive_tasks, data);
  return TRUE;
}

static void launch_progress_bar(const gchar *change_id, DsState *state) {
  SnapProgress *data = g_new0(SnapProgress, 1);
  data->state = state;
  data->change_id = g_strdup(change_id);
  data->busy = FALSE;
  data->timeout_id = g_timeout_add(CHANGE_CHECK_INTERVAL,
                                   (GSourceFunc)launch_progress_cb, data);
}

static void snap_dbus_handle_change(SnapChanges *object,
                                    const gchar *arg_change_id,
                                    const gchar *arg_change_type,
                                    GVariant *arg_metadata, DsState *state) {
  if (g_strcmp0(arg_change_type, "delayed-auto-refresh") == 0) {
    launch_progress_bar(arg_change_id, state);
  }
}

void snap_dbus_cb(GDBusConnection *connection, GAsyncResult *res,
                  DsState *state) {
  state->snap_dbus_proxy = snap_changes_proxy_new_finish(res, NULL);
  if (state->snap_dbus_proxy == NULL) {
    g_warning("Failed to connect to the snap userd agent DBus interface");
    return;
  }
  g_signal_connect(state->snap_dbus_proxy, "change",
                   G_CALLBACK(snap_dbus_handle_change), state);
}

void manage_snap_dbus(GDBusConnection *connection, DsState *state) {

  snap_changes_proxy_new(connection, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START,
                         "io.snapcraft.SessionAgent",
                         "/io/snapcraft/SnapChanges", NULL,
                         (GAsyncReadyCallback)snap_dbus_cb, state);
}
