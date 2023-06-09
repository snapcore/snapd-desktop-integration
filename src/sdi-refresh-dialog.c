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

#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "iresources.h"
#include "sdi-refresh-dialog.h"
#include "sdi-refresh-monitor-private.h"

#define ICON_SIZE 48

struct _SdiRefreshDialog {
  GtkWindow parent_instance;

  GtkLabel *message;
  GtkLabel *app_label;
  GtkProgressBar *progress_bar;
  GtkImage *app_icon;

  SdiRefreshMonitor *monitor;
  gchar *app_name;
  gchar *lock_file;
  guint timeout_id;
  guint close_id;
  gboolean pulsed;
  gboolean wait_change_in_lock_file;
};

G_DEFINE_TYPE(SdiRefreshDialog, sdi_refresh_dialog, GTK_TYPE_WINDOW)

static gboolean on_close_window(GtkWindow *self) {
  gtk_window_destroy(GTK_WINDOW(self));
  return TRUE;
}

static void hide_cb(SdiRefreshDialog *self) {
  gtk_window_destroy(GTK_WINDOW(self));
}

static gboolean refresh_progress_bar(SdiRefreshDialog *dialog) {
  struct stat statbuf;
  if (dialog->pulsed) {
    gtk_progress_bar_pulse(dialog->progress_bar);
  }
  if (dialog->lock_file == NULL) {
    return G_SOURCE_CONTINUE;
  }
  if (stat(dialog->lock_file, &statbuf) != 0) {
    if ((errno == ENOENT) || (errno == ENOTDIR)) {
      if (dialog->wait_change_in_lock_file) {
        return G_SOURCE_CONTINUE;
      }
      gtk_window_destroy(GTK_WINDOW(dialog));
      return G_SOURCE_REMOVE;
    }
  } else {
    if (statbuf.st_size == 0) {
      if (dialog->wait_change_in_lock_file) {
        return G_SOURCE_CONTINUE;
      }
      gtk_window_destroy(GTK_WINDOW(dialog));
      return G_SOURCE_REMOVE;
    }
  }
  // if we arrive here, we wait for the lock file to be empty
  dialog->wait_change_in_lock_file = FALSE;
  return G_SOURCE_CONTINUE;
}

static void set_message(SdiRefreshDialog *dialog, const gchar *message) {
  if (message == NULL)
    return;
  gtk_label_set_text(dialog->app_label, message);
}

static void set_title(SdiRefreshDialog *dialog, const gchar *title) {
  if (title == NULL)
    return;
  gtk_window_set_title(GTK_WINDOW(dialog), title);
}

static void set_icon(SdiRefreshDialog *dialog, const gchar *icon) {
  if (icon == NULL)
    return;
  if (strlen(icon) == 0) {
    gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), FALSE);
    return;
  }
  gtk_image_set_from_icon_name(dialog->app_icon, icon);
  gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), TRUE);
}

static void set_icon_image(SdiRefreshDialog *dialog, const gchar *path) {
  g_autoptr(GFile) fimage = NULL;
  g_autoptr(GdkPixbuf) image = NULL;
  g_autoptr(GdkPixbuf) final_image = NULL;
  gint scale;

  if (path == NULL)
    return;
  if (strlen(path) == 0) {
    gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), FALSE);
    return;
  }
  fimage = g_file_new_for_path(path);
  if (!g_file_query_exists(fimage, NULL)) {
    gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), FALSE);
    return;
  }
  // This convoluted code is needed to be able to scale
  // any picture to the desired size, and also to allow
  // to set the scale and take advantage of the monitor
  // scale.
  image = gdk_pixbuf_new_from_file(path, NULL);
  if (image == NULL) {
    gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), FALSE);
    return;
  }
  scale = gtk_widget_get_scale_factor(GTK_WIDGET(dialog->app_icon));
  final_image = gdk_pixbuf_scale_simple(image, ICON_SIZE * scale,
                                        ICON_SIZE * scale, GDK_INTERP_BILINEAR);
  gtk_image_set_from_pixbuf(dialog->app_icon, final_image);
  gtk_widget_set_visible(GTK_WIDGET(dialog->app_icon), TRUE);
}

static void set_desktop_file(SdiRefreshDialog *dialog, const gchar *path) {
  g_autoptr(GDesktopAppInfo) app_info = NULL;
  g_autofree gchar *icon = NULL;

  if (path == NULL)
    return;

  if (strlen(path) == 0)
    return;

  app_info = g_desktop_app_info_new_from_filename(path);
  if (app_info == NULL) {
    return;
  }
  // extract the icon from the desktop file
  icon = g_desktop_app_info_get_string(app_info, "Icon");
  if (icon != NULL)
    set_icon_image(dialog, icon);
}

static void handle_extra_params(SdiRefreshDialog *dialog,
                                GVariant *extra_params) {
  GVariantIter iter;
  GVariant *value;
  const gchar *key;

  // Do a copy to allow manage the iter in other places if needed
  g_variant_iter_init(&iter, extra_params);
  while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
    if (!g_strcmp0(key, "message")) {
      set_message(dialog, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "title")) {
      set_title(dialog, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "icon")) {
      set_icon(dialog, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "icon_image")) {
      set_icon_image(dialog, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "wait_change_in_lock_file")) {
      dialog->wait_change_in_lock_file = TRUE;
    } else if (!g_strcmp0(key, "desktop_file")) {
      set_desktop_file(dialog, g_variant_get_string(value, NULL));
    }
    g_variant_unref(value);
  }
}

static void sdi_refresh_dialog_dispose(GObject *object) {
  SdiRefreshDialog *self = SDI_REFRESH_DIALOG(object);

  sdi_refresh_monitor_remove_application(self->monitor, self);

  if (self->timeout_id != 0) {
    g_source_remove(self->timeout_id);
  }
  if (self->close_id != 0) {
    g_signal_handler_disconnect(self, self->close_id);
  }
  g_free(self->lock_file);
  g_clear_pointer(&self->app_name, g_free);

  G_OBJECT_CLASS(sdi_refresh_dialog_parent_class)->dispose(object);
}

static void sdi_refresh_dialog_init(SdiRefreshDialog *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
}

static void sdi_refresh_dialog_class_init(SdiRefreshDialogClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = sdi_refresh_dialog_dispose;

  gtk_widget_class_set_template_from_resource(
      GTK_WIDGET_CLASS(klass),
      "/io/snapcraft/SnapDesktopIntegration/sdi-refresh-dialog.ui");

  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiRefreshDialog, app_label);
  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiRefreshDialog, progress_bar);
  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiRefreshDialog, app_icon);

  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass), hide_cb);
}

void handle_application_is_being_refreshed(const gchar *app_name,
                                           const gchar *lock_file_path,
                                           GVariant *extra_params,
                                           SdiRefreshMonitor *monitor) {
  SdiRefreshDialog *dialog = NULL;
  g_autofree gchar *label_text = NULL;

  dialog = sdi_refresh_monitor_lookup_application(monitor, app_name);
  if (dialog != NULL) {
    gtk_window_present(GTK_WINDOW(dialog));
    handle_extra_params(dialog, extra_params);
    return;
  }

  dialog = sdi_refresh_dialog_new(monitor, app_name);
  if (*lock_file_path == 0) {
    dialog->lock_file = NULL;
  } else {
    dialog->lock_file = g_strdup(lock_file_path);
  }
  dialog->wait_change_in_lock_file = FALSE;
  label_text = g_strdup_printf(
      _("Refreshing “%s” to latest version. Please wait."), app_name);
  gtk_label_set_text(dialog->app_label, label_text);

  g_signal_connect(G_OBJECT(dialog), "close-request",
                   G_CALLBACK(on_close_window), dialog);

  dialog->timeout_id =
      g_timeout_add(200, G_SOURCE_FUNC(refresh_progress_bar), dialog);
  gtk_window_present(GTK_WINDOW(dialog));
  sdi_refresh_monitor_add_application(monitor, dialog);
  handle_extra_params(dialog, extra_params);
}

void handle_application_refresh_completed(const gchar *app_name,
                                          GVariant *extra_params,
                                          SdiRefreshMonitor *monitor) {
  SdiRefreshDialog *dialog = NULL;

  dialog = sdi_refresh_monitor_lookup_application(monitor, app_name);
  if (dialog == NULL) {
    return;
  }
  gtk_window_destroy(GTK_WINDOW(dialog));
}

void handle_set_pulsed_progress(const gchar *app_name, const gchar *bar_text,
                                GVariant *extra_params,
                                SdiRefreshMonitor *monitor) {
  SdiRefreshDialog *dialog = NULL;

  dialog = sdi_refresh_monitor_lookup_application(monitor, app_name);
  if (dialog == NULL) {
    return;
  }
  dialog->pulsed = TRUE;
  if ((bar_text == NULL) || (bar_text[0] == 0)) {
    gtk_progress_bar_set_show_text(dialog->progress_bar, FALSE);
  } else {
    gtk_progress_bar_set_show_text(dialog->progress_bar, TRUE);
    gtk_progress_bar_set_text(dialog->progress_bar, bar_text);
  }
  handle_extra_params(dialog, extra_params);
}

void handle_set_percentage_progress(const gchar *app_name,
                                    const gchar *bar_text, gdouble percent,
                                    GVariant *extra_params,
                                    SdiRefreshMonitor *monitor) {
  SdiRefreshDialog *dialog = NULL;

  dialog = sdi_refresh_monitor_lookup_application(monitor, app_name);
  if (dialog == NULL) {
    return;
  }
  dialog->pulsed = FALSE;
  gtk_progress_bar_set_fraction(dialog->progress_bar, percent);
  gtk_progress_bar_set_show_text(dialog->progress_bar, TRUE);
  if ((bar_text != NULL) && (bar_text[0] == 0)) {
    gtk_progress_bar_set_text(dialog->progress_bar, NULL);
  } else {
    gtk_progress_bar_set_text(dialog->progress_bar, bar_text);
  }
  handle_extra_params(dialog, extra_params);
}

SdiRefreshDialog *sdi_refresh_dialog_new(SdiRefreshMonitor *monitor,
                                         const gchar *app_name) {
  SdiRefreshDialog *self = g_object_new(sdi_refresh_dialog_get_type(), NULL);

  self->app_name = g_strdup(app_name);
  self->monitor = monitor;
  self->pulsed = TRUE;

  return self;
}

const gchar *sdi_refresh_dialog_get_app_name(SdiRefreshDialog *self) {
  return self->app_name;
}
