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

#include "refresh_status.h"
#include "iresources.h"
#include <cairo.h>
#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <libintl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

gboolean on_close_window(GtkWindow *self, RefreshState *state) {
  refresh_state_free(state);
  return TRUE;
}

void on_hide_clicked(GtkButton *button, RefreshState *state) {
  refresh_state_free(state);
}

static gboolean refresh_progress_bar(RefreshState *state) {
  struct stat statbuf;
  if (state->pulsed) {
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->progressBar));
  }
  if (state->lockFile == NULL) {
    return G_SOURCE_CONTINUE;
  }
  if (stat(state->lockFile, &statbuf) != 0) {
    if ((errno == ENOENT) || (errno == ENOTDIR)) {
      if (state->wait_change_in_lock_file) {
        return G_SOURCE_CONTINUE;
      }
      refresh_state_free(state);
      return G_SOURCE_REMOVE;
    }
  } else {
    if (statbuf.st_size == 0) {
      if (state->wait_change_in_lock_file) {
        return G_SOURCE_CONTINUE;
      }
      refresh_state_free(state);
      return G_SOURCE_REMOVE;
    }
  }
  // if we arrive here, we wait for the lock file to be empty
  state->wait_change_in_lock_file = FALSE;
  return G_SOURCE_CONTINUE;
}

RefreshState *ds_state_find_application(DsState *state, const char *appName) {
  GList *list = state->refreshing_list;
  for (; list != NULL; list = list->next) {
    RefreshState *state = (RefreshState *)list->data;
    if (0 == g_strcmp0(state->appName->str, appName)) {
      return state;
    }
  }
  return NULL;
}

static void set_message(RefreshState *state, const gchar *message) {
  if (message == NULL)
    return;
  gtk_label_set_text(state->message, message);
}

static void set_title(RefreshState *state, const gchar *title) {
  if (title == NULL)
    return;
  gtk_window_set_title(GTK_WINDOW(state->window), title);
}

static void set_icon(RefreshState *state, const gchar *icon) {
  if (icon == NULL)
    return;
  if (strlen(icon) == 0) {
    gtk_widget_set_visible(state->icon, FALSE);
    return;
  }
  gtk_image_set_from_icon_name(GTK_IMAGE(state->icon), icon);
  gtk_widget_set_visible(state->icon, TRUE);
}

static void set_icon_image(RefreshState *state, const gchar *path) {
  g_autoptr(GFile) fimage = NULL;
  g_autoptr(GdkPixbuf) image = NULL;
  g_autoptr(GdkPixbuf) final_image = NULL;
  gint scale;

  if (path == NULL)
    return;
  if (strlen(path) == 0) {
    gtk_widget_set_visible(state->icon, FALSE);
    return;
  }
  fimage = g_file_new_for_path(path);
  if (!g_file_query_exists(fimage, NULL)) {
    gtk_widget_set_visible(state->icon, FALSE);
    return;
  }
  // This convoluted code is needed to be able to scale
  // any picture to the desired size, and also to allow
  // to set the scale and take advantage of the monitor
  // scale.
  image = gdk_pixbuf_new_from_file(path, NULL);
  if (image == NULL) {
    gtk_widget_set_visible(state->icon, FALSE);
    return;
  }
  scale = gtk_widget_get_scale_factor(GTK_WIDGET(state->icon));
  final_image = gdk_pixbuf_scale_simple(image, ICON_SIZE * scale,
                                        ICON_SIZE * scale, GDK_INTERP_BILINEAR);
  gtk_image_set_from_pixbuf(GTK_IMAGE(state->icon), final_image);
  gtk_widget_set_visible(state->icon, TRUE);
}

static void set_desktop_file(RefreshState *state, const gchar *path) {
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
    set_icon_image(state, icon);
}

static void handle_extra_params(RefreshState *state, GVariant *extraParams) {
  GVariantIter iter;
  GVariant *value;
  gchar *key;

  if (extraParams == NULL) {
    return;
  }

  // Do a copy to allow manage the iter in other places if needed
  g_variant_iter_init(&iter, extraParams);
  while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
    if (!g_strcmp0(key, "message")) {
      set_message(state, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "title")) {
      set_title(state, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "icon")) {
      set_icon(state, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "icon_image")) {
      set_icon_image(state, g_variant_get_string(value, NULL));
    } else if (!g_strcmp0(key, "wait_change_in_lock_file")) {
      state->wait_change_in_lock_file = TRUE;
    } else if (!g_strcmp0(key, "desktop_file")) {
      set_desktop_file(state, g_variant_get_string(value, NULL));
    }
    g_variant_unref(value);
    g_free(key);
  }
}

void handle_application_is_being_refreshed(gchar *appName, gchar *lockFilePath,
                                           GVariant *extraParams,
                                           DsState *ds_state) {
  RefreshState *state = NULL;
  g_autoptr(GtkWidget) container = NULL;
  g_autoptr(GtkWidget) label = NULL;
  g_autoptr(GString) labelText = NULL;
  g_autoptr(GtkBuilder) builder = NULL;
  GtkButton *button;

  state = ds_state_find_application(ds_state, appName);
  if (state != NULL) {
    gtk_window_present(GTK_WINDOW(state->window));
    handle_extra_params(state, extraParams);
    return;
  }

  state = refresh_state_new(ds_state, appName);
  if (*lockFilePath == 0) {
    state->lockFile = NULL;
  } else {
    state->lockFile = g_strdup(lockFilePath);
  }
  state->wait_change_in_lock_file = FALSE;
  builder = gtk_builder_new_from_resource(
      "/io/snapcraft/SnapDesktopIntegration/snap_is_being_refreshed_gtk4.ui");
  state->window = GTK_APPLICATION_WINDOW(
      g_object_ref(gtk_builder_get_object(builder, "main_window")));
  state->message =
      GTK_LABEL(g_object_ref(gtk_builder_get_object(builder, "app_label")));
  state->progressBar =
      GTK_WIDGET(g_object_ref(gtk_builder_get_object(builder, "progress_bar")));
  state->icon =
      GTK_WIDGET(g_object_ref(gtk_builder_get_object(builder, "app_icon")));
  button = GTK_BUTTON(gtk_builder_get_object(builder, "button_hide"));
  labelText = g_string_new("");
  g_string_printf(
      labelText, _("Refreshing “%s” to latest version. Please wait."), appName);
  gtk_label_set_text(state->message, labelText->str);

  g_signal_connect(G_OBJECT(state->window), "close-request",
                   G_CALLBACK(on_close_window), state);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(on_hide_clicked),
                   state);

  gtk_widget_set_visible(state->icon, FALSE);

  state->timeoutId =
      g_timeout_add(200, G_SOURCE_FUNC(refresh_progress_bar), state);
  gtk_window_present(GTK_WINDOW(state->window));
  ds_state->refreshing_list = g_list_append(ds_state->refreshing_list, state);
  handle_extra_params(state, extraParams);
}

void handle_close_application_window(gchar *appName, GVariant *extraParams,
                                     DsState *ds_state) {
  RefreshState *state = NULL;

  state = ds_state_find_application(ds_state, appName);
  if (state == NULL) {
    return;
  }
  refresh_state_free(state);
}

void handle_set_pulsed_progress(gchar *appName, gchar *barText,
                                GVariant *extraParams, DsState *ds_state) {
  RefreshState *state = NULL;

  state = ds_state_find_application(ds_state, appName);
  if (state == NULL) {
    return;
  }
  state->pulsed = TRUE;
  if ((barText == NULL) || (barText[0] == 0)) {
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progressBar), FALSE);
  } else {
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progressBar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progressBar), barText);
  }
  handle_extra_params(state, extraParams);
}

void handle_set_percentage_progress(gchar *appName, gchar *barText,
                                    gdouble percent, GVariant *extraParams,
                                    DsState *ds_state) {
  RefreshState *state = NULL;

  state = ds_state_find_application(ds_state, appName);
  if (state == NULL) {
    return;
  }
  state->pulsed = FALSE;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progressBar), percent);
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progressBar), TRUE);
  if ((barText != NULL) && (barText[0] == 0)) {
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progressBar), NULL);
  } else {
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progressBar), barText);
  }
  handle_extra_params(state, extraParams);
}

RefreshState *refresh_state_new(DsState *state, gchar *appName) {
  RefreshState *object = g_new0(RefreshState, 1);
  object->appName = g_string_new(appName);
  object->dsstate = state;
  object->pulsed = TRUE;
  object->width = 0;
  object->height = 0;
  return object;
}

void refresh_state_free(RefreshState *state) {

  DsState *dsstate = state->dsstate;

  dsstate->refreshing_list = g_list_remove(dsstate->refreshing_list, state);

  if (state->timeoutId != 0) {
    g_source_remove(state->timeoutId);
  }
  if (state->closeId != 0) {
    g_signal_handler_disconnect(G_OBJECT(state->window), state->closeId);
  }
  g_free(state->lockFile);
  g_clear_object(&state->progressBar);
  g_string_free(state->appName, TRUE);
  if (state->window != NULL) {
    gtk_window_destroy(GTK_WINDOW(state->window));
  }
  g_free(state->summary);
  g_clear_object(&state->window);
  g_free(state);
}
