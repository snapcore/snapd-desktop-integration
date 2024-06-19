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

#define SECONDS_IN_A_DAY 86400
#define SECONDS_IN_AN_HOUR 3600
#define SECONDS_IN_A_MINUTE 60

#include <gio/gdesktopappinfo.h>
#include <glib/gi18n.h>
#include <libnotify/notify.h>

#include "sdi-helpers.h"
#include "sdi-notify.h"

enum { PROP_APPLICATION = 1, PROP_LAST };

struct _SdiNotify {
  GObject parent_instance;

  GApplication *application;
};

G_DEFINE_TYPE(SdiNotify, sdi_notify, G_TYPE_OBJECT)

static void sdi_notify_action_ignore(GActionGroup *action_group,
                                     GVariant *app_list, SdiNotify *self) {
  gsize len;
  g_autofree gchar **apps = (gchar **)g_variant_get_strv(app_list, &len);

  g_return_if_fail(apps != NULL);
  for (gsize i = 0; i < len; i++)
    g_signal_emit_by_name(self, "ignore-snap-event", apps[i]);
}

static GTimeSpan get_remaining_time(SnapdSnap *snap) {
  g_autoptr(GDateTime) proceed_time = snapd_snap_get_proceed_time(snap);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  return g_date_time_difference(proceed_time, now);
}

static GVariant *get_snap_list(GSList *snaps) {
  if (snaps == NULL)
    return NULL;
  g_autoptr(GVariantBuilder) builder =
      g_variant_builder_new(G_VARIANT_TYPE("as"));
  for (; snaps != NULL; snaps = snaps->next) {
    SnapdSnap *snap = (SnapdSnap *)snaps->data;
    g_variant_builder_add(builder, "s", snapd_snap_get_name(snap));
  }
  return g_variant_ref_sink(g_variant_builder_end(builder));
}

// Currently, due to the way Snapd creates the .desktop files, the notifications
// created with GNotify don't show the application icon in the upper-left
// corner, putting instead the "generic gears" icon. This is the reason why, by
// default, we use libnotify. This problem is being tackled by the snapd people,
// so, in the future, GNotify would be the prefered choice.
#ifndef USE_GNOTIFY

static gchar *get_icon_name_from_gicon(GIcon *icon) {
  if (icon == NULL) {
    return NULL;
  }

  if (G_IS_THEMED_ICON(icon)) {
    GThemedIcon *themed_icon = G_THEMED_ICON(icon);
    const gchar *const *names = g_themed_icon_get_names(themed_icon);
    if (names == NULL)
      return NULL;
    return g_strdup(names[0]);
  }

  if (G_IS_FILE_ICON(icon)) {
    GFileIcon *file_icon = G_FILE_ICON(icon);
    // the value returned by g_file_icon_get_file() is owned by the instance
    return g_file_get_path(g_file_icon_get_file(file_icon));
  }
  return NULL;
}

typedef struct {
  SdiNotify *self;
  GVariant *snaps;
} IgnoreNotifyData;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NotifyNotification, g_object_unref)

static void app_close_notification(NotifyNotification *notification,
                                   char *action, gpointer user_data) {
  g_object_unref(notification);
}

static void app_ignore_snaps_notification(NotifyNotification *notification,
                                          char *action, gpointer user_data) {
  IgnoreNotifyData *data = user_data;
  sdi_notify_action_ignore(NULL, data->snaps, data->self);
  g_variant_unref(data->snaps);
  g_free(user_data);

  g_object_unref(notification);
}

static void show_pending_update_notification(SdiNotify *self,
                                             const gchar *title,
                                             const gchar *body, GIcon *icon,
                                             GSList *snaps) {
  g_autofree gchar *icon_name = get_icon_name_from_gicon(icon);
  // Don't use g_autoptr because it must survive for the actions
  NotifyNotification *notification =
      notify_notification_new(title, body, icon_name);
  if (icon_name != NULL) {
    // don't use g_autoptr with the GVariant because it is consumed in set_hint
    notify_notification_set_hint(notification, "image-path",
                                 g_variant_new_string(icon_name));
  }
  notify_notification_add_action(notification, "app.close-notification",
                                 _("Close"), app_close_notification, NULL,
                                 NULL);
  notify_notification_add_action(notification, "default", _("Close"),
                                 app_close_notification, NULL, NULL);
  IgnoreNotifyData *data = g_malloc(sizeof(IgnoreNotifyData));
  data->self = self;
  data->snaps = get_snap_list(snaps);
  notify_notification_add_action(notification, "app.ignore-notification",
                                 _("Ignore"), app_ignore_snaps_notification,
                                 data, NULL);
  notify_notification_show(notification, NULL);
}

static void show_simple_notification(SdiNotify *self, const gchar *title,
                                     const gchar *body, GIcon *icon,
                                     const gchar *id) {
  g_autofree gchar *icon_name = get_icon_name_from_gicon(icon);
  // Don't use g_autoptr because it must survive for the actions
  NotifyNotification *notification =
      notify_notification_new(title, body, icon_name);

  if (icon_name != NULL) {
    notify_notification_set_hint(notification, "image-path",
                                 g_variant_new_string(icon_name));
  }
  notify_notification_add_action(notification, "default", _("Close"),
                                 app_close_notification, NULL, NULL);
  notify_notification_show(notification, NULL);
}

#else

static void show_pending_update_notification(SdiNotify *self,
                                             const gchar *title,
                                             const gchar *body, GIcon *icon,
                                             GSList *snaps) {

  g_autoptr(GNotification) notification = g_notification_new(title);
  g_notification_set_body(notification, body);
  if (icon != NULL) {
    g_notification_set_icon(notification, g_object_ref(icon));
  }
  g_notification_set_default_action_and_target(
      notification, "app.close-notification", "s", "pending-update");
  g_notification_add_button_with_target(notification, _("Close"),
                                        "app.close-notification", "s",
                                        "pending-update");
  g_autoptr(GVariant) snap_list = get_snap_list(snaps);
  g_notification_add_button_with_target_value(notification, _("Ignore"),
                                              "app.ignore-updates", snap_list);
  g_application_send_notification(self->application, "pending-update",
                                  notification);
}

static void show_simple_notification(SdiNotify *self, const gchar *title,
                                     const gchar *body, GIcon *icon,
                                     const gchar *id) {
  g_autoptr(GNotification) notification = g_notification_new(title);
  g_notification_set_body(notification, body);
  if (icon != NULL) {
    g_notification_set_icon(notification, g_object_ref(icon));
  }
  g_application_send_notification(self->application, id, notification);
}
#endif

void sdi_notify_pending_refresh_one(SdiNotify *self, SnapdSnap *snap) {
  g_return_if_fail(SDI_IS_NOTIFY(self));
  g_return_if_fail(snap != NULL);

  g_autoptr(GAppInfo) app_info = sdi_get_desktop_file_from_snap(snap);

  const gchar *name = snapd_snap_get_name(snap);
  if (app_info != NULL) {
    name = g_app_info_get_display_name(app_info);
  }

  g_autofree gchar *title =
      g_strdup_printf(_("Pending update of %s snap"), name);

  GTimeSpan difference = get_remaining_time(snap) / 1000000;

  g_autofree gchar *body = NULL;
  if (difference > SECONDS_IN_A_DAY) {
    body = g_strdup_printf(_("Close the app to start updating (%ld days left)"),
                           difference / SECONDS_IN_A_DAY);
  } else if (difference > SECONDS_IN_AN_HOUR) {
    body =
        g_strdup_printf(_("Close the app to start updating (%ld hours left)"),
                        difference / SECONDS_IN_AN_HOUR);
  } else {
    body =
        g_strdup_printf(_("Close the app to start updating (%ld minutes left)"),
                        difference / SECONDS_IN_A_MINUTE);
  }

  GIcon *icon = NULL;
  if (app_info != NULL) {
    icon = g_app_info_get_icon(app_info);
  }

  show_pending_update_notification(self, title, body, icon,
                                   g_slist_append(NULL, snap));
}

void sdi_notify_pending_refresh_multiple(SdiNotify *self, GSList *snaps) {
  g_return_if_fail(SDI_IS_NOTIFY(self));
  g_return_if_fail(snaps != NULL);

  g_autoptr(GString) body = g_string_new(_("Close the apps to start updating"));
  for (; snaps != NULL; snaps = snaps->next) {
    SnapdSnap *snap = (SnapdSnap *)snaps->data;
    const gchar *name = snapd_snap_get_name(snap);
    g_autoptr(GAppInfo) app_info = sdi_get_desktop_file_from_snap(snap);
    if (app_info != NULL) {
      name = g_app_info_get_display_name(app_info);
    }

    GTimeSpan difference = get_remaining_time(snap) / 1000000;
    if (difference > 86400) {
      g_string_append_printf(body, _(" (%s %ld days left)"), name,
                             difference / 86400);
    } else if (difference > 3600) {
      g_string_append_printf(body, _(" (%s %ld hours left)"), name,
                             difference / 3600);
    } else {
      g_string_append_printf(body, _(" (%s %ld minutes left)"), name,
                             difference / 60);
    }
  }
  g_autoptr(GIcon) icon = g_themed_icon_new("emblem-important-symbolic");
  show_pending_update_notification(self, _("Pending updates for some snaps"),
                                   body->str, g_steal_pointer(&icon), snaps);
}

void sdi_notify_refresh_complete(SdiNotify *self, SnapdSnap *snap,
                                 const gchar *snap_name) {
  g_return_if_fail(SDI_IS_NOTIFY(self));
  g_return_if_fail((snap != NULL) || (snap_name != NULL));

  GIcon *icon = NULL;
  g_autoptr(GAppInfo) app_info = NULL;
  const gchar *name = NULL;

  if (snap != NULL) {
    app_info = sdi_get_desktop_file_from_snap(snap);
    if (app_info != NULL) {
      name = g_app_info_get_display_name(app_info);
      icon = g_app_info_get_icon(app_info);
    }
    if (name == NULL)
      name = snapd_snap_get_name(snap);
  }
  if (name == NULL)
    name = snap_name;

  g_autofree gchar *title = g_strdup_printf(_("%s was updated"), name);

  show_simple_notification(self, title, _("Ready to launch"), icon,
                           "update-complete");
}

GApplication *sdi_notify_get_application(SdiNotify *self) {
  g_return_val_if_fail(SDI_IS_NOTIFY(self), NULL);
  return self->application;
}

static void set_actions(SdiNotify *self) {
  g_autoptr(GVariantType) type_ignore = g_variant_type_new("as");
  g_autoptr(GSimpleAction) action_ignore =
      g_simple_action_new("ignore-updates", type_ignore);
  g_action_map_add_action(G_ACTION_MAP(self->application),
                          G_ACTION(action_ignore));
  g_autoptr(GVariantType) type_close = g_variant_type_new("s");
  g_autoptr(GSimpleAction) action_close =
      g_simple_action_new("close-notification", type_close);
  g_action_map_add_action(G_ACTION_MAP(self->application),
                          G_ACTION(action_close));
  g_signal_connect(G_OBJECT(action_ignore), "activate",
                   (GCallback)sdi_notify_action_ignore, self);
}

static void sdi_notify_set_property(GObject *object, guint prop_id,
                                    const GValue *value, GParamSpec *pspec) {
  SdiNotify *self = SDI_NOTIFY(object);
  gpointer p;

  switch (prop_id) {
  case PROP_APPLICATION:
    g_clear_object(&self->application);
    p = g_value_get_object(value);
    if (p != NULL) {
      self->application = g_object_ref(p);
      set_actions(self);
    }
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void sdi_notify_get_property(GObject *object, guint prop_id,
                                    GValue *value, GParamSpec *pspec) {
  SdiNotify *self = SDI_NOTIFY(object);

  switch (prop_id) {
  case PROP_APPLICATION:
    g_value_set_object(value, self->application);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void sdi_notify_dispose(GObject *object) {
  SdiNotify *self = SDI_NOTIFY(object);

  g_clear_object(&self->application);

  G_OBJECT_CLASS(sdi_notify_parent_class)->dispose(object);
}

void sdi_notify_init(SdiNotify *self) {}

void sdi_notify_class_init(SdiNotifyClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

  gobject_class->set_property = sdi_notify_set_property;
  gobject_class->get_property = sdi_notify_get_property;
  gobject_class->dispose = sdi_notify_dispose;

  g_object_class_install_property(
      gobject_class, PROP_APPLICATION,
      g_param_spec_object("application", "application",
                          "GApplication associated with this notify",
                          G_TYPE_APPLICATION,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_signal_new("ignore-snap-event", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
               0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

SdiNotify *sdi_notify_new(GApplication *application) {
  g_return_val_if_fail(application != NULL, NULL);

  return g_object_new(SDI_TYPE_NOTIFY, "application", application, NULL);
}
