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

#include <glib/gi18n.h>

#include "sdi-apparmor-prompt-dialog.h"

struct _SdiApparmorPromptDialog {
  GtkWindow parent_instance;

  GtkImage *image;
  GtkLabel *header_label;
  GtkLabel *details_label;
  GtkLabel *more_information_link_label;
  GtkLabel *more_information_label;

  // The request this dialog is responding to.
  SnapdClient *client;
  SnapdPromptingRequest *request;

  // Snap metadata.
  SnapdSnap *snap;
  SnapdSnap *store_snap;

  // Metrics recorded on usage of dialog.
  GDateTime *create_time;

  GCancellable *cancellable;
};

G_DEFINE_TYPE(SdiApparmorPromptDialog, sdi_apparmor_prompt_dialog,
              GTK_TYPE_WINDOW)

static gchar *permissions_to_label(SnapdPromptingPermissionFlags permissions) {
  struct {
    SnapdPromptingPermissionFlags flag;
    const char *name;
  } flags_to_name[] = {
      {SNAPD_PROMPTING_PERMISSION_FLAGS_EXECUTE, "execute"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_WRITE, "write"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_READ, "read"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_APPEND, "append"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CREATE, "create"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_DELETE, "delete"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_OPEN, "open"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_RENAME, "rename"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_SET_ATTR, "set-attr"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_GET_ATTR, "get-attr"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_SET_CRED, "set-cred"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_GET_CRED, "get-cred"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CHANGE_MODE, "change-mode"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CHANGE_OWNER, "change-owner"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CHANGE_GROUP, "change-group"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_LOCK, "lock"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_EXECUTE_MAP, "execute-map"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_LINK, "link"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CHANGE_PROFILE, "change-profile"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_CHANGE_PROFILE_ON_EXEC,
       "change-profile-on-exec"},
      {SNAPD_PROMPTING_PERMISSION_FLAGS_NONE, NULL}};

  g_autoptr(GPtrArray) permission_names = g_ptr_array_new();
  for (size_t i = 0;
       flags_to_name[i].flag != SNAPD_PROMPTING_PERMISSION_FLAGS_NONE; i++) {
    if ((permissions & flags_to_name[i].flag) != 0) {
      g_ptr_array_add(permission_names, (gpointer)flags_to_name[i].name);
    }
  }
  g_ptr_array_add(permission_names, NULL);
  return g_strjoinv(", ", (GStrv)permission_names->pdata);
}

static void report_metrics(SdiApparmorPromptDialog *self) {}

static void response_cb(GObject *object, GAsyncResult *result,
                        gpointer user_data) {
  g_autoptr(GError) error = NULL;
  if (!snapd_client_prompting_respond_finish(SNAPD_CLIENT(object), result,
                                             &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      return;
    }
    g_warning("Failed to respond to prompting request: %s", error->message);
    return;
  }
}

static void respond(SdiApparmorPromptDialog *self,
                    SnapdPromptingOutcome outcome,
                    SnapdPromptingLifespan lifespan) {
  snapd_client_prompting_respond_async(
      self->client, snapd_prompting_request_get_id(self->request), outcome,
      lifespan, 0, snapd_prompting_request_get_path(self->request),
      snapd_prompting_request_get_permissions(self->request), self->cancellable,
      response_cb, self);

  report_metrics(self);

  // FIXME: Make inactivate and wait to be destroyed
  gtk_window_destroy(GTK_WINDOW(self));
}

static void always_allow_cb(SdiApparmorPromptDialog *self) {
  respond(self, SNAPD_PROMPTING_OUTCOME_ALLOW,
          SNAPD_PROMPTING_LIFESPAN_FOREVER);
}

static void deny_once_cb(SdiApparmorPromptDialog *self) {
  respond(self, SNAPD_PROMPTING_OUTCOME_DENY, SNAPD_PROMPTING_LIFESPAN_SINGLE);
}

static void more_options_cb(SdiApparmorPromptDialog *self) {}

static gboolean close_request_cb(SdiApparmorPromptDialog *self) {
  respond(self, SNAPD_PROMPTING_OUTCOME_DENY, SNAPD_PROMPTING_LIFESPAN_SINGLE);
  return FALSE;
}

static void more_info_cb(SdiApparmorPromptDialog *self, const gchar *uri) {
  gtk_widget_set_visible(
      GTK_WIDGET(self->more_information_label),
      !gtk_widget_get_visible(GTK_WIDGET(self->more_information_label)));
}

static void update_metadata(SdiApparmorPromptDialog *self) {
  const gchar *snap_name = snapd_prompting_request_get_snap(self->request);
  SnapdPromptingPermissionFlags permissions =
      snapd_prompting_request_get_permissions(self->request);
  const gchar *path = snapd_prompting_request_get_path(self->request);

  // gtk_image_set_from_icon_name(self->image, icon);

  const gchar *title =
      self->snap != NULL ? snapd_snap_get_title(self->snap) : NULL;
  g_autofree gchar *permissions_label = permissions_to_label(permissions);
  const gchar *label = title != NULL ? title : snap_name;
  g_autofree gchar *header_text = g_strdup_printf(
      _("Do you want to allow %s to have %s access to your %s?"), label,
      permissions_label, path);
  gtk_label_set_markup(self->header_label, header_text);

  g_autoptr(GPtrArray) more_info_lines = g_ptr_array_new_with_free_func(g_free);

  // Information about the publisher.
  const gchar *publisher_id =
      self->snap != NULL ? snapd_snap_get_publisher_id(self->snap) : NULL;
  const gchar *publisher_name =
      self->snap != NULL ? snapd_snap_get_publisher_display_name(self->snap)
                         : NULL;
  if (publisher_id != NULL) {
    g_autofree gchar *publisher_url =
        g_strdup_printf("https://snapcraft.io/publisher/%s", publisher_id);
    g_ptr_array_add(
        more_info_lines,
        g_strdup_printf("%s: <a href=\"%s\">%s (%s)</a>", _("Publisher"),
                        publisher_url,
                        publisher_name != NULL ? publisher_name : publisher_id,
                        publisher_id));
  }

  // Information about when last updated.
  const gchar *channel_name =
      self->snap != NULL ? snapd_snap_get_channel(self->snap) : NULL;
  SnapdChannel *channel =
      self->store_snap != NULL && channel_name != NULL
          ? snapd_snap_match_channel(self->store_snap, channel_name)
          : NULL;
  if (channel != NULL) {
    g_autofree gchar *last_updated_date =
        g_date_time_format(snapd_channel_get_released_at(channel), "%e %B %Y");
    g_ptr_array_add(
        more_info_lines,
        g_strdup_printf("%s: %s", _("Last updated"), last_updated_date));
  }

  // Link to store.
  g_autofree gchar *store_url =
      g_strdup_printf("https://snapcraft.io/%s", snap_name);
  g_autofree gchar *more_information_text = g_strdup_printf(
      "<a href=\"%s\">%s</a>", store_url, _("Find out more on the store"));

  // Form into a bullet list.
  g_autoptr(GString) more_info_text = g_string_new("");
  for (guint i = 0; i < more_info_lines->len; i++) {
    const gchar *line = g_ptr_array_index(more_info_lines, i);
    if (i != 0) {
      g_string_append(more_info_text, "\n");
    }
    g_string_append(more_info_text, " • ");
    g_string_append(more_info_text, line);
  }
  gtk_label_set_markup(self->more_information_label, more_info_text->str);
}

static void get_snap_cb(GObject *object, GAsyncResult *result,
                        gpointer user_data) {
  SdiApparmorPromptDialog *self = user_data;

  g_autoptr(GError) error = NULL;
  g_autoptr(SnapdSnap) snap =
      snapd_client_get_snap_finish(SNAPD_CLIENT(object), result, &error);
  if (snap == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Failed to get snap metadata: %s", error->message);
    }
    return;
  }

  self->snap = g_object_ref(snap);
  update_metadata(self);
}

static void get_store_snap_cb(GObject *object, GAsyncResult *result,
                              gpointer user_data) {
  SdiApparmorPromptDialog *self = user_data;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) snaps =
      snapd_client_find_finish(SNAPD_CLIENT(object), result, NULL, &error);
  if (snaps == NULL) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("Failed to get store snap metadata: %s", error->message);
    }
    return;
  }

  if (snaps->len != 1) {
    g_warning("Invalid number of snaps returned for store snap search");
    return;
  }

  self->store_snap = g_object_ref(g_ptr_array_index(snaps, 0));
  update_metadata(self);
}

static void sdi_apparmor_prompt_dialog_dispose(GObject *object) {
  SdiApparmorPromptDialog *self = SDI_APPARMOR_PROMPT_DIALOG(object);

  g_cancellable_cancel(self->cancellable);

  g_clear_object(&self->client);
  g_clear_object(&self->request);
  g_clear_object(&self->snap);
  g_clear_object(&self->store_snap);
  g_clear_pointer(&self->create_time, g_date_time_unref);
  g_clear_object(&self->cancellable);

  G_OBJECT_CLASS(sdi_apparmor_prompt_dialog_parent_class)->dispose(object);
}

void sdi_apparmor_prompt_dialog_init(SdiApparmorPromptDialog *self) {
  self->create_time = g_date_time_new_now_utc();
  self->cancellable = g_cancellable_new();
  gtk_widget_init_template(GTK_WIDGET(self));
}

void sdi_apparmor_prompt_dialog_class_init(
    SdiApparmorPromptDialogClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = sdi_apparmor_prompt_dialog_dispose;

  gtk_widget_class_set_template_from_resource(
      GTK_WIDGET_CLASS(klass),
      "/io/snapcraft/SnapDesktopIntegration/sdi-apparmor-prompt-dialog.ui");

  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiApparmorPromptDialog, image);
  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiApparmorPromptDialog, header_label);
  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiApparmorPromptDialog, details_label);
  gtk_widget_class_bind_template_child(GTK_WIDGET_CLASS(klass),
                                       SdiApparmorPromptDialog,
                                       more_information_link_label);
  gtk_widget_class_bind_template_child(
      GTK_WIDGET_CLASS(klass), SdiApparmorPromptDialog, more_information_label);
  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass),
                                          always_allow_cb);
  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass),
                                          deny_once_cb);
  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass),
                                          more_options_cb);
  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass),
                                          close_request_cb);
  gtk_widget_class_bind_template_callback(GTK_WIDGET_CLASS(klass),
                                          more_info_cb);
}

SdiApparmorPromptDialog *
sdi_apparmor_prompt_dialog_new(SnapdClient *client,
                               SnapdPromptingRequest *request) {
  SdiApparmorPromptDialog *self =
      g_object_new(sdi_apparmor_prompt_dialog_get_type(), NULL);

  self->client = g_object_ref(client);
  self->request = g_object_ref(request);

  g_autofree gchar *details_text =
      g_strdup_printf(_("Denying access would affect non-essential features "
                        "from working properly"));
  gtk_label_set_markup(self->details_label, details_text);

  g_autofree gchar *more_information_link_text =
      g_strdup_printf("<a href=\"toggle_info\">%s</a>", _("More information"));
  gtk_label_set_markup(self->more_information_link_label,
                       more_information_link_text);

  // Look up metadata for this snap.
  const gchar *snap_name = snapd_prompting_request_get_snap(self->request);
  snapd_client_get_snap_async(client, snap_name, self->cancellable, get_snap_cb,
                              self);
  snapd_client_find_async(client, SNAPD_FIND_FLAGS_MATCH_NAME, snap_name,
                          self->cancellable, get_store_snap_cb, self);
  update_metadata(self);

  return self;
}

SnapdPromptingRequest *
sdi_apparmor_prompt_dialog_get_request(SdiApparmorPromptDialog *self) {
  return self->request;
}
