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
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <libnotify/notify.h>
#include <locale.h>
#include <signal.h>
#include <snapd-glib/snapd-glib.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include "org.freedesktop.login1.Session.h"
#include "org.freedesktop.login1.h"
#include "sdi-refresh-monitor.h"
#include "sdi-theme-monitor.h"

static Login1Manager *login_manager = NULL;
static SnapdClient *client = NULL;
static GtkApplication *app = NULL;
static SdiThemeMonitor *theme_monitor = NULL;
static SdiRefreshMonitor *refresh_monitor = NULL;

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

static void new_session(Login1Manager *manager, const gchar *session_id,
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
  GError *error = NULL;
#ifndef USE_GNOTIFY
  notify_init("snapd-desktop-integration_snapd-desktop-integration");
#endif
  client = snapd_client_new();
  refresh_monitor = sdi_refresh_monitor_new(G_APPLICATION(object));
  if (!sdi_refresh_monitor_start(refresh_monitor, &error)) {
    g_message("Failed to export the DBus Desktop Integration API %s",
              error->message);
  }
}

static void do_activate(GObject *object, gpointer data) {
  // because, by default, there are no windows, so the application would quit
  g_application_hold(G_APPLICATION(app));

  if (snapd_socket_path != NULL) {
    snapd_client_set_socket_path(client, snapd_socket_path);
  } else if (g_getenv("SNAP") != NULL) {
    snapd_client_set_socket_path(client, "/run/snapd-snap.socket");
  }

  theme_monitor = sdi_theme_monitor_new(client);
  sdi_theme_monitor_start(theme_monitor);
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

  app = gtk_application_new("io.snapcraft.SnapDesktopIntegration",
                            G_APPLICATION_ALLOW_REPLACEMENT |
                                G_APPLICATION_REPLACE);
  g_signal_connect(G_OBJECT(app), "startup", G_CALLBACK(do_startup), NULL);
  g_signal_connect(G_OBJECT(app), "shutdown", G_CALLBACK(do_shutdown), NULL);
  g_signal_connect(G_OBJECT(app), "activate", G_CALLBACK(do_activate), NULL);

  g_application_add_main_option_entries(G_APPLICATION(app), entries);

  g_application_run(G_APPLICATION(app), argc, argv);

  // since it should never ends, if we reach here, we return 0 as error value to
  // ensure that systemd will relaunch it.
  return 0;
}
