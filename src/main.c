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

#include "config.h"
#include <errno.h>
#include <gtk/gtk.h>
#include <libintl.h>
#include <libnotify/notify.h>
#include <locale.h>
#include <signal.h>
#include <snapd-glib/snapd-glib.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include "changes.h"
#include "dbus.h"
#include "ds_state.h"
#include "org.freedesktop.login1.Session.h"
#include "org.freedesktop.login1.h"
#include "refresh_status.h"

/* Number of second to wait after a theme change before checking for installed
 * snaps. */
#define CHECK_THEME_TIMEOUT_SECONDS 1

static Login1Manager *login_manager = NULL;

static void ds_state_free(DsState *state) {
  g_clear_object(&state->skeleton);
  g_clear_object(&state->settings);
  g_clear_object(&state->client);
  g_clear_handle_id(&state->check_delay_timer_id, g_source_remove);
  g_clear_pointer(&state->gtk_theme_name, g_free);
  g_clear_pointer(&state->icon_theme_name, g_free);
  g_clear_pointer(&state->cursor_theme_name, g_free);
  g_clear_pointer(&state->sound_theme_name, g_free);
  g_clear_object(&state->install_notification);
  g_clear_object(&state->progress_notification);
  g_list_free_full(state->refreshing_list, (GDestroyNotify)refresh_state_free);
  g_free(state);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(DsState, ds_state_free);

static void install_themes_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  DsState *state = user_data;
  g_autoptr(GError) error = NULL;

  if (snapd_client_install_themes_finish(SNAPD_CLIENT(object), result,
                                         &error)) {
    g_message("Installation complete.\n");
    notify_notification_update(
        state->progress_notification, _("Installing missing theme snaps:"),
        /// TRANSLATORS: installing a missing theme snap succeed
        _("Complete."), "dialog-information");
  } else {
    g_message("Installation failed: %s\n", error->message);
    gchar *error_message;
    switch (error->code) {
    case SNAPD_ERROR_AUTH_CANCELLED:
      /// TRANSLATORS: installing a missing theme snap was cancelled by the user
      error_message = _("Canceled by the user.");
      break;
    default:
      /// TRANSLATORS: installing a missing theme snap failed
      error_message = _("Failed.");
      break;
    }
    notify_notification_update(state->progress_notification,
                               _("Installing missing theme snaps:"),
                               error_message, "dialog-information");
  }

  notify_notification_show(state->progress_notification, NULL);
  g_clear_object(&state->progress_notification);
}

static void notification_closed_cb(NotifyNotification *notification,
                                   DsState *state) {
  /* Notification has been closed: */
  g_clear_object(&state->install_notification);
}

static void notify_cb(NotifyNotification *notification, char *action,
                      gpointer user_data) {
  DsState *state = user_data;

  if ((strcmp(action, "yes") == 0) || (strcmp(action, "default") == 0)) {
    g_message("Installing missing theme snaps...\n");
    state->progress_notification = notify_notification_new(
        _("Installing missing theme snaps:"), "...", "dialog-information");
    notify_notification_show(state->progress_notification, NULL);

    g_autoptr(GPtrArray) gtk_theme_names = g_ptr_array_new();
    if (state->gtk_theme_status == SNAPD_THEME_STATUS_AVAILABLE) {
      g_ptr_array_add(gtk_theme_names, state->gtk_theme_name);
    }
    g_ptr_array_add(gtk_theme_names, NULL);
    g_autoptr(GPtrArray) icon_theme_names = g_ptr_array_new();
    if (state->icon_theme_status == SNAPD_THEME_STATUS_AVAILABLE) {
      g_ptr_array_add(icon_theme_names, state->icon_theme_name);
    }
    if (state->cursor_theme_status == SNAPD_THEME_STATUS_AVAILABLE) {
      g_ptr_array_add(icon_theme_names, state->cursor_theme_name);
    }
    g_ptr_array_add(icon_theme_names, NULL);
    g_autoptr(GPtrArray) sound_theme_names = g_ptr_array_new();
    if (state->sound_theme_status == SNAPD_THEME_STATUS_AVAILABLE) {
      g_ptr_array_add(sound_theme_names, state->sound_theme_name);
    }
    g_ptr_array_add(sound_theme_names, NULL);
    snapd_client_install_themes_async(
        state->client, (gchar **)gtk_theme_names->pdata,
        (gchar **)icon_theme_names->pdata, (gchar **)sound_theme_names->pdata,
        NULL, NULL, NULL, install_themes_cb, state);
  }
}

static void show_install_notification(DsState *state) {
  /* If we've already displayed a notification, do nothing */
  if (state->install_notification != NULL)
    return;

  state->install_notification = notify_notification_new(
      _("Some required theme snaps are missing."),
      _("Would you like to install them now?"), "dialog-question");
  g_signal_connect(state->install_notification, "closed",
                   G_CALLBACK(notification_closed_cb), state);
  notify_notification_set_timeout(state->install_notification,
                                  NOTIFY_EXPIRES_NEVER);
  notify_notification_add_action(
      state->install_notification, "yes",
      /// TRANSLATORS: answer to the question "Would you like to install them
      /// now?" referred to snap themes
      _("Yes"), notify_cb, state, NULL);
  notify_notification_add_action(
      state->install_notification, "no",
      /// TRANSLATORS: answer to the question "Would you like to install them
      /// now?" referred to snap themes
      _("No"), notify_cb, state, NULL);
  notify_notification_add_action(state->install_notification, "default",
                                 "default", notify_cb, state, NULL);

  notify_notification_show(state->install_notification, NULL);
}

static void check_themes_cb(GObject *object, GAsyncResult *result,
                            gpointer user_data) {
  DsState *state = user_data;

  g_autoptr(GHashTable) gtk_theme_status = NULL;
  g_autoptr(GHashTable) icon_theme_status = NULL;
  g_autoptr(GHashTable) sound_theme_status = NULL;
  g_autoptr(GError) error = NULL;
  if (!snapd_client_check_themes_finish(SNAPD_CLIENT(object), result,
                                        &gtk_theme_status, &icon_theme_status,
                                        &sound_theme_status, &error)) {
    g_warning("Could not check themes: %s", error->message);
    return;
  }

  if (state->gtk_theme_name)
    state->gtk_theme_status = GPOINTER_TO_INT(
        g_hash_table_lookup(gtk_theme_status, state->gtk_theme_name));
  else
    state->gtk_theme_status = SNAPD_THEME_STATUS_UNAVAILABLE;
  if (state->icon_theme_name)
    state->icon_theme_status = GPOINTER_TO_INT(
        g_hash_table_lookup(icon_theme_status, state->icon_theme_name));
  else
    state->icon_theme_status = SNAPD_THEME_STATUS_UNAVAILABLE;
  if (state->cursor_theme_name)
    state->cursor_theme_status = GPOINTER_TO_INT(
        g_hash_table_lookup(icon_theme_status, state->cursor_theme_name));
  else
    state->cursor_theme_status = SNAPD_THEME_STATUS_UNAVAILABLE;
  if (state->sound_theme_name)
    state->sound_theme_status = GPOINTER_TO_INT(
        g_hash_table_lookup(sound_theme_status, state->sound_theme_name));
  else
    state->sound_theme_status = SNAPD_THEME_STATUS_UNAVAILABLE;

  gboolean themes_available =
      state->gtk_theme_status == SNAPD_THEME_STATUS_AVAILABLE ||
      state->icon_theme_status == SNAPD_THEME_STATUS_AVAILABLE ||
      state->cursor_theme_status == SNAPD_THEME_STATUS_AVAILABLE ||
      state->sound_theme_status == SNAPD_THEME_STATUS_AVAILABLE;

  if (!themes_available) {
    g_message("All available theme snaps installed\n");
    return;
  }

  g_message("Missing theme snaps\n");

  show_install_notification(state);
}

static gboolean get_themes_cb(DsState *state) {
  state->check_delay_timer_id = 0;

  g_autofree gchar *gtk_theme_name = NULL;
  g_autofree gchar *icon_theme_name = NULL;
  g_autofree gchar *cursor_theme_name = NULL;
  g_autofree gchar *sound_theme_name = NULL;
  g_object_get(state->settings, "gtk-theme-name", &gtk_theme_name,
               "gtk-icon-theme-name", &icon_theme_name, "gtk-cursor-theme-name",
               &cursor_theme_name, "gtk-sound-theme-name", &sound_theme_name,
               NULL);

  /* If nothing has changed, we're done */
  if (g_strcmp0(state->gtk_theme_name, gtk_theme_name) == 0 &&
      g_strcmp0(state->icon_theme_name, icon_theme_name) == 0 &&
      g_strcmp0(state->cursor_theme_name, cursor_theme_name) == 0 &&
      g_strcmp0(state->sound_theme_name, sound_theme_name) == 0) {
    return G_SOURCE_REMOVE;
  }

  g_message("New theme: gtk=%s icon=%s cursor=%s, sound=%s", gtk_theme_name,
            icon_theme_name, cursor_theme_name, sound_theme_name);

  g_free(state->gtk_theme_name);
  state->gtk_theme_name = g_steal_pointer(&gtk_theme_name);
  state->gtk_theme_status = 0;

  g_free(state->icon_theme_name);
  state->icon_theme_name = g_steal_pointer(&icon_theme_name);
  state->icon_theme_status = 0;

  g_free(state->cursor_theme_name);
  state->cursor_theme_name = g_steal_pointer(&cursor_theme_name);
  state->cursor_theme_status = 0;

  g_free(state->sound_theme_name);
  state->sound_theme_name = g_steal_pointer(&sound_theme_name);
  state->sound_theme_status = 0;

  g_autoptr(GPtrArray) gtk_theme_names = g_ptr_array_new();
  if (state->gtk_theme_name)
    g_ptr_array_add(gtk_theme_names, state->gtk_theme_name);
  g_ptr_array_add(gtk_theme_names, NULL);

  g_autoptr(GPtrArray) icon_theme_names = g_ptr_array_new();
  if (state->icon_theme_name)
    g_ptr_array_add(icon_theme_names, state->icon_theme_name);
  if (state->cursor_theme_name)
    g_ptr_array_add(icon_theme_names, state->cursor_theme_name);
  g_ptr_array_add(icon_theme_names, NULL);

  g_autoptr(GPtrArray) sound_theme_names = g_ptr_array_new();
  if (state->sound_theme_name)
    g_ptr_array_add(sound_theme_names, state->sound_theme_name);
  g_ptr_array_add(sound_theme_names, NULL);

  snapd_client_check_themes_async(
      state->client, (gchar **)gtk_theme_names->pdata,
      (gchar **)icon_theme_names->pdata, (gchar **)sound_theme_names->pdata,
      NULL, check_themes_cb, state);

  return G_SOURCE_REMOVE;
}

static void queue_check_theme(DsState *state) {
  /* Delay processing the theme, in case multiple themes are being changed at
   * the same time. */
  g_clear_handle_id(&state->check_delay_timer_id, g_source_remove);
  state->check_delay_timer_id = g_timeout_add_seconds(
      CHECK_THEME_TIMEOUT_SECONDS, G_SOURCE_FUNC(get_themes_cb), state);
}

static gchar *snapd_socket_path = NULL;

static GOptionEntry entries[] = {{"snapd-socket-path", 0, 0,
                                  G_OPTION_ARG_FILENAME, &snapd_socket_path,
                                  "Snapd socket path", "PATH"},
                                 {NULL}};

static gboolean session_is_desktop(const gchar *object_path) {
  g_autoptr(OrgFreedesktopLogin1Session) session = NULL;
  g_autoptr(GVariant) user = NULL;
  GVariant *user_data =
      NULL; // this value belongs to the session proxy, so it must not be freed
  const gchar *session_type =
      NULL; // this value belongs to the session proxy, so it must not be freed

  session = org_freedesktop_login1_session_proxy_new_for_bus_sync(
      G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.freedesktop.login1",
      object_path, NULL, NULL);
  user_data = org_freedesktop_login1_session_get_user(session);
  if (user_data == NULL) {
    g_message("Failed to read the session user data. Forcing a reload.");
    // if we can't read the data, we can't know whether we are in a desktop
    // session or in a text one, so we will assume that we are in a session
    // desktop to force a reload.
    return TRUE;
  }
  user = g_variant_get_child_value(user_data, 0);
  if (user == NULL) {
    g_message("Failed to read the session user.");
    return FALSE;
  }
  if (getuid() != g_variant_get_uint32(user)) {
    // the new session isn't for our user
    return FALSE;
  }
  session_type = org_freedesktop_login1_session_get_type_(session);
  if (session_type == NULL) {
    g_message("Failed to read the session type");
    return FALSE;
  }
  if (!g_strcmp0("x11", session_type) || !g_strcmp0("wayland", session_type) ||
      !g_strcmp0("mir", session_type)) {
    // this is a graphical session
    return TRUE;
  }
  return FALSE;
}

static void new_session(Login1Manager *manager, gchar *session_id,
                        const gchar *object_path, gpointer data) {
  GMainLoop *loop = (GMainLoop *)data;
  g_message("Detected new session %s at %s\n", session_id, object_path);

  if (session_is_desktop(object_path)) {
    g_message("The new session is of desktop type. Relaunching "
              "snapd-desktop-integration.");
    g_main_loop_quit(loop);
  }
}

static gboolean check_graphical_sessions(gpointer data) {
  GMainLoop *loop = (GMainLoop *)data;

  GVariant *sessions = NULL;
  gboolean got_session_list;

  got_session_list = login1_manager_call_list_sessions_sync(
      login_manager, &sessions, NULL, NULL);

  if (got_session_list) {
    // check if there is already a graphical session opened for us, in which
    // case we must just exit and let systemd to relaunch us, because it means
    // that we run too early and the desktop wasn't still ready
    for (int i = 0; i < g_variant_n_children(sessions); i++) {
      GVariant *session = g_variant_get_child_value(sessions, i);
      if (session == NULL) {
        continue;
      }
      GVariant *session_object_variant = g_variant_get_child_value(session, 4);
      const gchar *session_object =
          g_variant_get_string(session_object_variant, NULL);
      g_message("Checking session %s...", session_object);
      if (session_is_desktop(session_object)) {
        g_message("Is a desktop session! Forcing a reload.");
        g_main_loop_quit(loop);
      }
      g_variant_unref(session_object_variant);
      g_variant_unref(session);
    }
  } else {
    g_message("Failed to get session list (check that login-session-observe "
              "interface is connected). Forcing a reload.");
    g_main_loop_quit(loop);
  }
  return G_SOURCE_REMOVE;
}
static void do_startup(GObject *object, gpointer data) {
  DsState *state = (DsState *)data;
  notify_init("snapd-desktop-integration");
  state->snap_dbus_proxy = NULL;
  state->settings = gtk_settings_get_default();
  state->client = snapd_client_new();
  state->app = GTK_APPLICATION(object);
  if (!register_dbus(
          g_application_get_dbus_connection(G_APPLICATION(state->app)), state,
          NULL)) {
    g_clear_object(&(state->skeleton));
    g_message("Failed to export the DBus Desktop Integration API");
  }
  manage_snap_dbus(g_application_get_dbus_connection(G_APPLICATION(state->app)),
                   state);
}

static void do_activate(GObject *object, gpointer data) {
  DsState *state = (DsState *)data;

  // because, by default, there are no windows, so the application would quit
  g_application_hold(G_APPLICATION(state->app));

  if (snapd_socket_path != NULL) {
    snapd_client_set_socket_path(state->client, snapd_socket_path);
  } else if (g_getenv("SNAP") != NULL) {
    snapd_client_set_socket_path(state->client, "/run/snapd-snap.socket");
  }

  /* Listen for theme changes. */
  g_signal_connect_swapped(state->settings, "notify::gtk-theme-name",
                           G_CALLBACK(queue_check_theme), state);
  g_signal_connect_swapped(state->settings, "notify::gtk-icon-theme-name",
                           G_CALLBACK(queue_check_theme), state);
  g_signal_connect_swapped(state->settings, "notify::gtk-cursor-theme-name",
                           G_CALLBACK(queue_check_theme), state);
  g_signal_connect_swapped(state->settings, "notify::gtk-sound-theme-name",
                           G_CALLBACK(queue_check_theme), state);
  get_themes_cb(state);
}

static void do_shutdown(GObject *object, gpointer data) { notify_uninit(); }

static int global_retval = 0;

void sighandler(int v) {
  global_retval = 128 + v; // exit value is usually 128 + signal_id
}

int main(int argc, char **argv) {
  int retval;
  int pid = fork();
  if (pid != 0) {
    if (pid > 0) {
      // SIGTERM and SIGINT will be ignored, but
      // will break WAITPID with a EINTR value in
      // errno.
      // This allows the program to always exit with
      // a NO-ERROR value, and kill the child
      struct sigaction signal_data;
      signal_data.sa_handler = sighandler;
      sigemptyset(&signal_data.sa_mask);
      signal_data.sa_flags = 0;
      sigaction(SIGTERM, &signal_data, NULL);
      sigaction(SIGINT, &signal_data, NULL);

      retval = waitpid(pid, NULL, 0);
      if ((retval < 0) && (errno == EINTR)) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
      }
    }
    return global_retval;
  }

  setlocale(LC_ALL, "");
  bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
  textdomain(GETTEXT_PACKAGE);

  if (!gtk_init_check()) {
    g_message("Failed to do gtk init. Waiting for a new session with desktop "
              "capabilities.");

    GMainLoop *loop = g_main_loop_new(NULL, TRUE);
    login_manager = login1_manager_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, "org.freedesktop.login1",
        "/org/freedesktop/login1", NULL, NULL);
    g_signal_connect(login_manager, "session-new", G_CALLBACK(new_session),
                     loop);
    // Check if we are already in a graphical session to avoid race conditions
    // between the signals being connected and the main loop being run. This is
    // a must because, sometimes, snapd-desktop-integration is launched "too
    // quickly" and the desktop isn't ready, so gtk_init_check() fails but the
    // session IS a graphical one. For that cases we do check if there is a
    // graphical session active, and if that's the case, we must exit to let
    // systemd relaunch us again, this time being able to get access to the
    // session.
    g_idle_add(check_graphical_sessions, loop);
    g_main_loop_run(loop);
    g_message("Loop exited. Forcing reload.");
    return 0;
  }

  g_autoptr(GtkApplication) app = NULL;
  g_autoptr(DsState) state = g_new0(DsState, 1);

  app = gtk_application_new("io.snapcraft.SnapDesktopIntegration",
                            G_APPLICATION_ALLOW_REPLACEMENT |
                                G_APPLICATION_REPLACE);
  g_signal_connect(G_OBJECT(app), "startup", G_CALLBACK(do_startup), state);
  g_signal_connect(G_OBJECT(app), "shutdown", G_CALLBACK(do_shutdown), state);
  g_signal_connect(G_OBJECT(app), "activate", G_CALLBACK(do_activate), state);

  g_application_add_main_option_entries(G_APPLICATION(app), entries);

  g_application_run(G_APPLICATION(app), argc, argv);

  // since it should never ends, if we reach here, we return 0 as error value to
  // ensure that systemd will relaunch it.
  return 0;
}
