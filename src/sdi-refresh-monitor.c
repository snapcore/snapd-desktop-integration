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

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <snapd-glib/snapd-glib.h>
#include <unistd.h>

#include "com.canonical.Unity.LauncherEntry.h"
#include "sdi-helpers.h"
#include "sdi-notify.h"
#include "sdi-refresh-monitor.h"

// time in ms for periodic check
#define CHANGE_REFRESH_PERIOD 500

static void error_cb(GObject *object, GError *error, gpointer data);
static void manage_change_update(SnapdClient *source, GAsyncResult *res,
                                 gpointer p);

enum { PROP_NOTIFY = 1, PROP_LAST };

struct _SdiRefreshMonitor {
  GObject parent_instance;

  SdiNotify *notify;
  GHashTable *snaps;
  GHashTable *changes;
  SnapdNoticesMonitor *snapd_monitor;
  SnapdClient *client;
  guint signal_notice_id;
  guint signal_error_id;
  GtkWindow *main_window;
  GApplication *application;
  GtkBox *refresh_bar_container;
  UnityComCanonicalUnityLauncherEntry *unity_manager;
  GHashTable *refreshing_snap_list;
};

G_DEFINE_TYPE(SdiRefreshMonitor, sdi_refresh_monitor, G_TYPE_OBJECT)

typedef struct {
  gchar *change_id;
  gchar *snap_name;
  SdiRefreshMonitor *self;
} SnapRefreshData;

static void free_change_refresh_data(SnapRefreshData *data) {
  g_free(data->change_id);
  g_free(data->snap_name);
  g_clear_object(&data->self);
  g_free(data);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SnapRefreshData, free_change_refresh_data);

typedef struct {
  guint total_tasks;
  guint done_tasks;
  gdouble old_progress;
  gboolean done;
  GPtrArray *desktop_files;
} SnapProgressTaskData;

static void free_progress_task_data(void *data) {
  SnapProgressTaskData *p = data;
  g_ptr_array_unref(p->desktop_files);
  g_free(data);
}

static SnapProgressTaskData *new_progress_task_data(const gchar *snap_name) {
  SnapProgressTaskData *retval = g_malloc0(sizeof(SnapProgressTaskData));
  retval->old_progress = -1;
  retval->done = FALSE;
  retval->desktop_files = sdi_get_desktop_filenames_for_snap(snap_name);
  return retval;
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(SnapProgressTaskData, free_progress_task_data);

static SdiSnap *find_snap(SdiRefreshMonitor *self, const gchar *snap_name) {
  SdiSnap *snap =
      (SdiSnap *)g_hash_table_lookup(self->snaps, (gconstpointer)snap_name);
  return (snap == NULL) ? NULL : g_object_ref(snap);
}

static SdiSnap *add_snap(SdiRefreshMonitor *self, const gchar *snap_name) {
  g_autoptr(SdiSnap) snap = find_snap(self, snap_name);
  if (snap == NULL) {
    snap = sdi_snap_new(snap_name);
    g_hash_table_insert(self->snaps, (gpointer)snap_name, g_object_ref(snap));
  }
  return g_steal_pointer(&snap);
}

static gboolean contains_child(GtkWidget *parent, GtkWidget *query_child) {
  GtkWidget *child = gtk_widget_get_first_child(parent);
  while (child != NULL) {
    if (query_child == child)
      return TRUE;
    child = gtk_widget_get_next_sibling(child);
  }
  return FALSE;
}

static void remove_dialog(SdiRefreshMonitor *self, SdiRefreshDialog *dialog) {
  if (dialog == NULL)
    return;

  if (!contains_child(GTK_WIDGET(self->refresh_bar_container),
                      GTK_WIDGET(dialog)))
    return;

  gtk_box_remove(GTK_BOX(self->refresh_bar_container), GTK_WIDGET(dialog));
  if (gtk_widget_get_first_child(GTK_WIDGET(self->refresh_bar_container)) ==
      NULL) {
    gtk_window_destroy(self->main_window);
    self->main_window = NULL;
  } else {
    gtk_window_set_default_size(GTK_WINDOW(self->main_window), 0, 0);
  }
}

static void remove_snap(SdiRefreshMonitor *self, SdiSnap *snap) {
  if (snap == NULL)
    return;
  g_autoptr(SdiRefreshDialog) dialog = sdi_snap_get_dialog(snap);
  remove_dialog(self, dialog);
  g_hash_table_remove(self->snaps, sdi_snap_get_name(snap));
}

void close_dialog_cb(SdiRefreshDialog *dialog, SdiRefreshMonitor *self) {
  g_autofree gchar *snap_name =
      g_strdup(sdi_refresh_dialog_get_app_name(dialog));
  g_autoptr(SdiSnap) snap = find_snap(self, snap_name);
  sdi_snap_set_manually_hidden(snap, TRUE);
  remove_dialog(self, dialog);
  sdi_snap_set_dialog(snap, NULL);
}

static void add_dialog_to_main_window(SdiRefreshMonitor *self,
                                      SdiRefreshDialog *dialog) {
  if (self->main_window == NULL) {
    self->main_window = GTK_WINDOW(
        gtk_application_window_new(GTK_APPLICATION(self->application)));
    gtk_window_set_deletable(self->main_window, FALSE);
    self->refresh_bar_container =
        GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    gtk_window_set_child(self->main_window,
                         GTK_WIDGET(self->refresh_bar_container));
    gtk_window_set_title(GTK_WINDOW(self->main_window), _("Refreshing snaps"));
    gtk_window_present(GTK_WINDOW(self->main_window));
    gtk_window_set_default_size(GTK_WINDOW(self->main_window), 0, 0);
  }
  gtk_box_append(self->refresh_bar_container, GTK_WIDGET(dialog));
  gtk_widget_set_visible(GTK_WIDGET(dialog), TRUE);
  g_signal_connect(G_OBJECT(dialog), "hide-event", (GCallback)close_dialog_cb,
                   self);
}

static void begin_application_refresh(GObject *source, GAsyncResult *res,
                                      gpointer p) {

  g_autoptr(SnapRefreshData) data = p;
  g_autoptr(SdiRefreshMonitor) self = g_steal_pointer(&data->self);
  g_autoptr(GError) error = NULL;

  g_autoptr(SnapdSnap) snap =
      snapd_client_get_snap_finish(SNAPD_CLIENT(source), res, &error);

  if ((error != NULL) &&
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    return;
  }

  if ((error != NULL) || (snap == NULL)) {
    // Snapd doesn't have the patch to allow to access GET_SNAPS/{name}; use
    // generic data
    g_autoptr(SdiSnap) sdi_snap = add_snap(self, data->snap_name);
    g_autoptr(SdiRefreshDialog) dialog = sdi_snap_get_dialog(sdi_snap);
    // This check is a must, in case the call is too slow and the timer call
    // this twice. It would be rare, because the timer is 1/3 of a second,
    // but... just in case.
    if (dialog == NULL) {
      dialog = g_object_ref(
          sdi_refresh_dialog_new(data->snap_name, data->snap_name));
      add_dialog_to_main_window(self, dialog);
      sdi_snap_set_dialog(sdi_snap, dialog);
    }
    return;
  }

  g_autoptr(SdiSnap) sdi_snap = add_snap(self, snapd_snap_get_name(snap));
  g_autoptr(SdiRefreshDialog) dialog = sdi_snap_get_dialog(sdi_snap);

  if (dialog == NULL) {
    const gchar *snap_name = snapd_snap_get_name(snap);
    const gchar *visible_name = NULL;
    g_autoptr(GAppInfo) app_info = sdi_get_desktop_file_from_snap(snap);
    g_autofree gchar *icon = NULL;
    if (app_info != NULL) {
      visible_name = g_app_info_get_display_name(G_APP_INFO(app_info));
      icon =
          g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(app_info), "Icon");
    }
    if (visible_name == NULL)
      visible_name = snap_name;
    dialog = g_object_ref(sdi_refresh_dialog_new(snap_name, visible_name));
    if (icon != NULL)
      sdi_refresh_dialog_set_icon_image(dialog, icon);
    add_dialog_to_main_window(self, dialog);
    sdi_snap_set_dialog(sdi_snap, dialog);
  }
}

static void show_snap_completed(GObject *source, GAsyncResult *res,
                                gpointer p) {
  g_autoptr(SnapRefreshData) data = p;
  g_autoptr(SdiRefreshMonitor) self = g_object_ref(data->self);
  g_autoptr(GError) error = NULL;

  g_autoptr(SnapdSnap) snap =
      snapd_client_get_snap_finish(SNAPD_CLIENT(source), res, &error);
  if ((error != NULL) &&
      (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
    return;
  }
  if ((error == NULL) && (snap != NULL))
    sdi_notify_refresh_complete(self->notify, snap, NULL);
  else
    sdi_notify_refresh_complete(self->notify, NULL, data->snap_name);
}

static void refresh_change(gpointer p) {
  g_autoptr(SnapRefreshData) data = p;
  snapd_client_get_change_async(data->self->client, data->change_id, NULL,
                                (GAsyncReadyCallback)manage_change_update,
                                g_object_ref(data->self));
}

static gboolean status_is_done(const gchar *status) {
  gboolean done = g_str_equal(status, "Done") | g_str_equal(status, "Abort") |
                  g_str_equal(status, "Error") | g_str_equal(status, "Hold") |
                  g_str_equal(status, "Wait");
  return done;
}

static void update_inhibited_snaps(SdiRefreshMonitor *self, SnapdChange *change,
                                   gboolean done, gboolean hold) {
  SnapdAutorefreshChangeData *changeData =
      SNAPD_AUTOREFRESH_CHANGE_DATA(snapd_change_get_data(change));

  if (changeData == NULL) {
    return;
  }

  GStrv snapNames = snapd_autorefresh_change_data_get_snap_names(changeData);
  for (gchar **p = snapNames; *p != NULL; p++) {
    g_autoptr(SdiSnap) snap = find_snap(self, *p);
    // Only show progress bar if that snap shown an 'inhibited' notification
    if (snap == NULL)
      continue;

    if (!sdi_snap_get_inhibited(snap))
      continue;

    if (done || hold) {
      remove_snap(self, snap);
      SnapRefreshData *data = g_malloc0(sizeof(SnapRefreshData));
      data->snap_name = g_strdup(*p);
      data->self = g_object_ref(self);
      if (done) {
        snapd_client_get_snap_async(self->client, *p, NULL, show_snap_completed,
                                    data);
      }
      continue;
    }

    if (sdi_snap_get_hidden(snap) || sdi_snap_get_manually_hidden(snap)) {
      continue;
    }

    g_autoptr(SdiRefreshDialog) dialog = sdi_snap_get_dialog(snap);
    if (dialog == NULL) {
      // if there's no dialog, get the data for this snap and create
      // it avoid refresh notifications while the progress dialog is shown
      sdi_snap_set_ignored(snap, TRUE);
      SnapRefreshData *data = g_malloc0(sizeof(SnapRefreshData));
      data->snap_name = g_strdup(*p);
      data->self = g_object_ref(self);
      snapd_client_get_snap_async(self->client, *p, NULL,
                                  begin_application_refresh, data);
      continue;
    }

    // if there's already a dialog for this snap, just refresh the
    // progress bar
    GPtrArray *tasks = snapd_change_get_tasks(change);
    gint done = 0;
    g_autoptr(SnapdTask) current_task = NULL;
    for (guint i = 0; i < tasks->len; i++) {
      SnapdTask *task = (SnapdTask *)tasks->pdata[i];
      const gchar *status = snapd_task_get_status(task);
      if (status_is_done(status)) {
        done++;
      } else if ((current_task == NULL) && g_str_equal("Doing", status)) {
        current_task = g_object_ref(task);
      }
    }
    if (current_task != NULL) {
      sdi_refresh_dialog_set_n_tasks_progress(
          dialog, snapd_task_get_summary(current_task), done, tasks->len);
    }
  }
}

static void update_dock_bar(gpointer key, gpointer value, gpointer data) {
  SnapProgressTaskData *task_data = value;
  g_autoptr(SdiRefreshMonitor) self = data;

  gdouble progress =
      ((gdouble)task_data->done_tasks) / ((gdouble)task_data->total_tasks);
  if ((progress == task_data->old_progress) && !task_data->done) {
    return;
  }
  task_data->done_tasks = 0;
  task_data->total_tasks = 0;
  task_data->old_progress = progress;
  g_autoptr(GVariantBuilder) builder =
      g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(builder, "{sv}", "progress",
                        g_variant_new_double(progress));
  g_variant_builder_add(builder, "{sv}", "progress-visible",
                        g_variant_new_boolean(!task_data->done));
  g_variant_builder_add(builder, "{sv}", "updating",
                        g_variant_new_boolean(!task_data->done));
  // values has a floating reference, so it's consumed when calling
  // EMIT_UPDATE()
  GVariant *values = g_variant_builder_end(builder);

  for (int i = 0; i < task_data->desktop_files->len; i++) {
    const gchar *desktop_file = task_data->desktop_files->pdata[i];
    unity_com_canonical_unity_launcher_entry_emit_update(self->unity_manager,
                                                         desktop_file, values);
  }
}

static void update_dock_snaps(SdiRefreshMonitor *self, SnapdChange *change,
                              gboolean done, gboolean hold) {
  GPtrArray *tasks = snapd_change_get_tasks(change);
  GSList *snaps_to_remove = NULL;

  for (gint i = 0; i < tasks->len; i++) {
    SnapdTask *task = tasks->pdata[i];
    SnapdTaskData *task_data = snapd_task_get_data(task);
    if (task_data == NULL) {
      continue;
    }
    GStrv affected_snaps = snapd_task_data_get_affected_snaps(task_data);
    if (affected_snaps == NULL) {
      continue;
    }
    gboolean task_done = status_is_done(snapd_task_get_status(task));
    for (gchar **p = affected_snaps; *p != NULL; p++) {
      gchar *snap_name = *p;
      SnapProgressTaskData *progress_task_data = NULL;
      if (!g_hash_table_contains(self->refreshing_snap_list, snap_name)) {
        progress_task_data = new_progress_task_data(snap_name);
        g_hash_table_insert(self->refreshing_snap_list, g_strdup(snap_name),
                            progress_task_data);
      } else {
        progress_task_data =
            g_hash_table_lookup(self->refreshing_snap_list, snap_name);
      }
      progress_task_data->total_tasks++;
      progress_task_data->done = task_done;
      if (task_done) {
        progress_task_data->done_tasks++;
      }
      if (done || hold) {
        snaps_to_remove = g_slist_prepend(snaps_to_remove, g_strdup(snap_name));
      }
    }
  }
  g_hash_table_foreach(self->refreshing_snap_list, update_dock_bar,
                       g_object_ref(self));
  for (GSList *p = snaps_to_remove; p != NULL; p = p->next) {
    g_hash_table_remove(self->refreshing_snap_list, p->data);
  }
  g_slist_free_full(snaps_to_remove, g_free);
}

static gboolean cancelled_change_status(const gchar *status) {
  return g_str_equal(status, "Hold") || g_str_equal(status, "Undone") ||
         g_str_equal(status, "Undo");
}

static void manage_change_update(SnapdClient *source, GAsyncResult *res,
                                 gpointer p) {
  g_autoptr(SdiRefreshMonitor) self = p;
  g_autoptr(GError) error = NULL;
  g_autoptr(SnapdChange) change =
      snapd_client_get_change_finish(source, res, &error);

  if (error != NULL) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      return;
    }
    g_print("Error in manage_change_update: %s\n", error->message);
    return;
  }
  if (change == NULL)
    return;

  gboolean done = g_str_equal(snapd_change_get_status(change), "Done");
  gboolean cancelled = cancelled_change_status(snapd_change_get_status(change));

  if (g_str_equal(snapd_change_get_kind(change), "auto-refresh")) {
    update_inhibited_snaps(self, change, done, cancelled);
  }
  update_dock_snaps(self, change, done, cancelled);

  if (!done && !cancelled) {
    // refresh periodically this data, until the snap has been refreshed
    SnapRefreshData *data = g_malloc0(sizeof(SnapRefreshData));
    data->change_id = g_strdup(snapd_change_get_id(change));
    data->self = g_object_ref(self);
    g_timeout_add_once(CHANGE_REFRESH_PERIOD, (GSourceOnceFunc)refresh_change,
                       data);
  }
}

static void manage_refresh_inhibit(SnapdClient *source, GAsyncResult *res,
                                   gpointer p) {
  g_autoptr(SdiRefreshMonitor) self = p;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) snaps =
      snapd_client_get_snaps_finish(source, res, &error);

  if (error != NULL) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      return;
    }
    g_print("Error in refresh inhibit: %s\n", error->message);
    return;
  }
  if (snaps->len == 0)
    return;
  // remove snaps that are marked as "ignore"
  g_autoptr(GSList) snaps_not_ignored = NULL;
  for (guint i = 0; i < snaps->len; i++) {
    const gchar *name = snapd_snap_get_name(snaps->pdata[i]);
    if (name == NULL)
      continue;
    g_autoptr(SdiSnap) snap_data = add_snap(self, name);
    if (snap_data == NULL)
      continue;
    sdi_snap_set_inhibited(snap_data, TRUE);
    if (sdi_snap_get_ignored(snap_data))
      continue;
    snaps_not_ignored =
        g_slist_prepend(snaps_not_ignored, g_object_ref(snaps->pdata[i]));
  }
  if (g_slist_length(snaps_not_ignored) > 1) {
    sdi_notify_pending_refresh_multiple(self->notify, snaps_not_ignored);
    return;
  }
  if (snaps_not_ignored != NULL) {
    // just one notice
    sdi_notify_pending_refresh_one(self->notify, snaps_not_ignored->data);
    return;
  }
}

static void notice_cb(GObject *object, SnapdNotice *notice, gboolean first_run,
                      gpointer monitor) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(monitor);

  g_autoptr(GHashTable) data = snapd_notice_get_last_data(notice);
  g_autofree gchar *kind = g_strdup(g_hash_table_lookup(data, "kind"));

  switch (snapd_notice_get_notice_type(notice)) {
  case SNAPD_NOTICE_TYPE_CHANGE_UPDATE:
    /**
     * During first run, we must ignore these events to avoid showing old
     * notices that do not apply anymore.
     */
    if (first_run)
      return;
    if (!g_str_equal(kind, "auto-refresh") &&
        !g_str_equal(kind, "refresh-snap"))
      return;
    snapd_client_get_change_async(
        self->client, snapd_notice_get_key(notice), NULL,
        (GAsyncReadyCallback)manage_change_update, g_object_ref(self));
    break;
  case SNAPD_NOTICE_TYPE_REFRESH_INHIBIT:
    snapd_client_get_snaps_async(
        self->client, SNAPD_GET_SNAPS_FLAGS_REFRESH_INHIBITED, NULL, NULL,
        (GAsyncReadyCallback)manage_refresh_inhibit, g_object_ref(self));
    break;
  case SNAPD_NOTICE_TYPE_SNAP_RUN_INHIBIT:
    // TODO. At this moment, no notice of this kind is emmited.
    break;
  default:
    break;
  }
}

SdiNotify *sdi_refresh_monitor_get_notify(SdiRefreshMonitor *self) {
  g_return_val_if_fail(SDI_IS_REFRESH_MONITOR(self), NULL);
  return self->notify;
}

static void sdi_refresh_monitor_dispose(GObject *object) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(object);

  if (self->snapd_monitor) {
    snapd_notices_monitor_stop(self->snapd_monitor, NULL);
    g_signal_handler_disconnect(self->snapd_monitor, self->signal_notice_id);
    g_signal_handler_disconnect(self->snapd_monitor, self->signal_error_id);
  }
  g_clear_pointer(&self->snaps, g_hash_table_unref);
  g_clear_object(&self->client);
  g_clear_object(&self->snapd_monitor);
  g_clear_object(&self->notify);
  g_clear_object(&self->application);
  g_clear_pointer(&self->refreshing_snap_list, g_hash_table_unref);
  if (self->main_window != NULL) {
    gtk_window_destroy(self->main_window);
    self->main_window = NULL;
  }

  G_OBJECT_CLASS(sdi_refresh_monitor_parent_class)->dispose(object);
}

static gboolean check_is_running_in_snap() {
  return g_getenv("SNAP_NAME") != NULL;
}

static void configure_snapd_monitor(SdiRefreshMonitor *self) {
  g_autoptr(SnapdClient) client = snapd_client_new();
  if (check_is_running_in_snap())
    snapd_client_set_socket_path(client, "/run/snapd-snap.socket");
  self->snapd_monitor = snapd_notices_monitor_new_with_client(client);
  self->signal_notice_id = g_signal_connect(self->snapd_monitor, "notice-event",
                                            (GCallback)notice_cb, self);
  self->signal_error_id = g_signal_connect(self->snapd_monitor, "error-event",
                                           (GCallback)error_cb, self);
}

static void launch_snapd_monitor_after_error(gpointer data) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(data);
  configure_snapd_monitor(self);
  snapd_notices_monitor_start(self->snapd_monitor, NULL);
}

static void error_cb(GObject *object, GError *error, gpointer data) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(data);
  g_print("Error %d; %s\n", error->code, error->message);
  g_signal_handler_disconnect(self->snapd_monitor, self->signal_notice_id);
  g_signal_handler_disconnect(self->snapd_monitor, self->signal_error_id);
  g_clear_object(&self->snapd_monitor);
  // wait one second to ensure that, in case that the error is because snapd is
  // being replaced, the new instance has created the new socket, and thus avoid
  // hundreds of error messages until it appears.
  g_timeout_add_once(1000, launch_snapd_monitor_after_error, self);
}

void sdi_refresh_monitor_init(SdiRefreshMonitor *self) {
  self->snaps =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  // the key in this table is the snap name; the value is a SnapProgressTaskData
  // structure
  self->refreshing_snap_list = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, free_progress_task_data);
  self->client = snapd_client_new();
  if (check_is_running_in_snap())
    snapd_client_set_socket_path(self->client, "/run/snapd-snap.socket");
  configure_snapd_monitor(self);
}

static void ignore_snap_cb(GObject *obj, const gchar *snap_name,
                           SdiRefreshMonitor *self) {
  g_autoptr(SdiSnap) snap = add_snap(self, snap_name);
  sdi_snap_set_ignored(snap, TRUE);
}

static void sdi_refresh_monitor_set_property(GObject *object, guint prop_id,
                                             const GValue *value,
                                             GParamSpec *pspec) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(object);
  gpointer p;

  switch (prop_id) {
  case PROP_NOTIFY:
    g_clear_object(&self->notify);
    p = g_value_get_object(value);
    if (p != NULL) {
      self->notify = g_object_ref(p);
      g_signal_connect(self->notify, "ignore-snap-event",
                       (GCallback)ignore_snap_cb, self);
    }
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void sdi_refresh_monitor_get_property(GObject *object, guint prop_id,
                                             GValue *value, GParamSpec *pspec) {
  SdiRefreshMonitor *self = SDI_REFRESH_MONITOR(object);

  switch (prop_id) {
  case PROP_NOTIFY:
    g_value_set_object(value, self->notify);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

void sdi_refresh_monitor_class_init(SdiRefreshMonitorClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

  gobject_class->set_property = sdi_refresh_monitor_set_property;
  gobject_class->get_property = sdi_refresh_monitor_get_property;
  gobject_class->dispose = sdi_refresh_monitor_dispose;

  g_object_class_install_property(
      gobject_class, PROP_NOTIFY,
      g_param_spec_object("notify", "notify", "Notify object", SDI_TYPE_NOTIFY,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

SdiRefreshMonitor *sdi_refresh_monitor_new(GApplication *application) {

  g_autoptr(SdiNotify) notify = sdi_notify_new(application);
  SdiRefreshMonitor *self =
      g_object_new(SDI_TYPE_REFRESH_MONITOR, "notify", notify, NULL);
  self->application = g_object_ref(application);
  g_autofree gchar *unity_object =
      g_strdup_printf("/com/canonical/unity/launcherentry/%d", getpid());
  self->unity_manager = unity_com_canonical_unity_launcher_entry_skeleton_new();
  g_dbus_interface_skeleton_export(
      G_DBUS_INTERFACE_SKELETON(self->unity_manager),
      g_application_get_dbus_connection(application), unity_object, NULL);
  return self;
}

gboolean sdi_refresh_monitor_start(SdiRefreshMonitor *self, GError **error) {

  g_return_val_if_fail(SDI_IS_REFRESH_MONITOR(self), FALSE);

  snapd_notices_monitor_start(self->snapd_monitor, NULL);
  return TRUE;
}
