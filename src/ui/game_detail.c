/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "game_detail.h"
#include "install_dialog.h"
#include "../installer_engine.h"
#include "../launch.h"
#include "../text_util.h"

struct _GogGameDetail
{
  GtkBox parent_instance;

  GtkWindow *parent_window;
  GogLibrary *library;
  GogConfig *config;
  GogState *state;

  GogGame *game; /* owned (reffed): a rescan replaces the library's objects */

  GtkPicture *logo;
  GtkLabel *title_label;
  GtkLabel *badges_label;
  GtkBox *action_box;
  GtkLabel *description_label;
  GtkBox *extras_box;
  GtkBox *dlc_box;
  AdwToastOverlay *toast_overlay;
};

enum
{
  SIG_CHANGED,
  N_SIGNALS,
};

static guint detail_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GogGameDetail, gog_game_detail, GTK_TYPE_BOX)

static void
gog_game_detail_dispose (GObject *object)
{
  GogGameDetail *self = GOG_GAME_DETAIL (object);
  g_clear_object (&self->game);
  G_OBJECT_CLASS (gog_game_detail_parent_class)->dispose (object);
}

static void
gog_game_detail_class_init (GogGameDetailClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gog_game_detail_dispose;
  detail_signals[SIG_CHANGED] =
      g_signal_new (GOG_GAME_DETAIL_SIGNAL_CHANGED, G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gog_game_detail_init (GogGameDetail *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}

static void refresh (GogGameDetail *self);

static void
emit_changed (GogGameDetail *self)
{
  g_signal_emit (self, detail_signals[SIG_CHANGED], 0);
}

static void
toast (GogGameDetail *self, const char *msg)
{
  adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (msg));
}

static void
on_install_done (GogGame *game, gboolean success, gpointer user_data)
{
  GogGameDetail *self = user_data;
  (void) game;
  if (success)
    toast (self, "Installed successfully");
  refresh (self);
  emit_changed (self);
}

static void
on_install_clicked (GtkButton *button, gpointer user_data)
{
  GogInstallerInfo *installer = g_object_get_data (G_OBJECT (button), "installer");
  GogGameDetail *self = user_data;
  GogGame *target = g_object_get_data (G_OBJECT (button), "dlc-game");
  if (!target)
    target = self->game;
  gog_show_install_dialog (self->parent_window, target, installer, self->config, self->state,
                            on_install_done, self);
}

static void
on_play_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  GError *error = NULL;
  if (!gog_launch_game (self->game, &error))
    {
      toast (self, error ? error->message : "Could not launch game");
      g_clear_error (&error);
    }
  else
    toast (self, "Launching…");
}

static void
on_uninstall_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (source);
  GogGameDetail *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (dialog, res);
  if (g_strcmp0 (response, "uninstall") != 0)
    return;

  gchar *removed_path = g_strdup (gog_game_get_install_path (self->game));
  gboolean is_dlc_record = gog_game_get_base_game (self->game) &&
                           g_strcmp0 (gog_game_get_install_path (gog_game_get_base_game (self->game)),
                                      removed_path) == 0;

  GError *error = NULL;
  if (!gog_uninstall_game (self->game, &error))
    {
      toast (self, error ? error->message : "Uninstall failed");
      g_clear_error (&error);
      g_free (removed_path);
      return;
    }
  gog_state_remove (self->state, gog_game_get_id (self->game));

  /* DLC installed into this game's directory went with it. */
  if (!is_dlc_record && removed_path)
    {
      GListModel *model = gog_library_get_model (self->library);
      guint n = g_list_model_get_n_items (model);
      for (guint i = 0; i < n; i++)
        {
          GogGame *g = g_list_model_get_item (model, i);
          if (g != self->game && gog_game_get_installed (g) &&
              g_strcmp0 (gog_game_get_install_path (g), removed_path) == 0)
            {
              gog_game_set_installed (g, FALSE);
              gog_game_set_install_path (g, NULL);
              gog_game_set_runner_kind (g, GOG_RUNNER_NONE);
              gog_game_set_runner_path (g, NULL);
              gog_game_set_prefix_path (g, NULL);
              gog_state_remove (self->state, gog_game_get_id (g));
            }
          g_object_unref (g);
        }
    }
  g_free (removed_path);

  GError *save_err = NULL;
  if (!gog_state_save (self->state, &save_err))
    {
      toast (self, save_err->message);
      g_clear_error (&save_err);
      return;
    }
  toast (self, is_dlc_record ? "DLC marked as not installed" : "Uninstalled");
  refresh (self);
  emit_changed (self);
}

static void
on_uninstall_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  GogGame *base = gog_game_get_base_game (self->game);
  gboolean is_dlc_record = base && g_strcmp0 (gog_game_get_install_path (base),
                                              gog_game_get_install_path (self->game)) == 0;
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
      is_dlc_record
          ? adw_alert_dialog_new ("Uninstall DLC?",
                                  "This DLC was installed into its base game's folder and its "
                                  "files can't be separated from the game's, so they stay on disk. "
                                  "It will only be marked as not installed.")
          : adw_alert_dialog_new ("Uninstall Game?",
                                  "This removes the installed game files (and its Wine/Proton "
                                  "prefix, if any) from disk, including any DLC installed into "
                                  "it. The original installer files are not affected."));
  adw_alert_dialog_add_responses (dialog, "cancel", "Cancel", "uninstall", "Uninstall", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "uninstall", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  adw_alert_dialog_choose (dialog, GTK_WIDGET (self->parent_window), NULL, on_uninstall_response, self);
}

static void
on_reveal_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  const char *path = gog_game_get_install_path (self->game);
  if (!path)
    return;
  GFile *file = g_file_new_for_path (path);
  gchar *uri = g_file_get_uri (file);
  GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);
  gtk_uri_launcher_launch (launcher, self->parent_window, NULL, NULL, NULL);
  g_object_unref (launcher);
  g_free (uri);
  g_object_unref (file);
}

static void
on_winecfg_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  GError *error = NULL;
  if (!gog_open_winecfg (self->game, &error))
    toast (self, error ? error->message : "Could not open winecfg");
  g_clear_error (&error);
}

static void
extra_path_closure_notify (gpointer data, GClosure *closure)
{
  (void) closure;
  g_free (data);
}

static void
open_extra_file (GtkWidget *button, const char *path)
{
  (void) button;
  GFile *file = g_file_new_for_path (path);
  gchar *uri = g_file_get_uri (file);
  GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);
  gtk_uri_launcher_launch (launcher, NULL, NULL, NULL, NULL);
  g_object_unref (launcher);
  g_free (uri);
  g_object_unref (file);
}

static void
clear_box (GtkBox *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (box))))
    gtk_box_remove (box, child);
}

static void
build_action_row (GogGameDetail *self)
{
  clear_box (self->action_box);

  if (gog_game_get_installed (self->game))
    {
      GtkWidget *play = gtk_button_new_with_label ("Play");
      gtk_widget_add_css_class (play, "suggested-action");
      gtk_widget_add_css_class (play, "pill");
      g_signal_connect (play, "clicked", G_CALLBACK (on_play_clicked), self);
      gtk_box_append (self->action_box, play);

      GtkWidget *uninstall = gtk_button_new_with_label ("Uninstall");
      gtk_widget_add_css_class (uninstall, "destructive-action");
      gtk_widget_add_css_class (uninstall, "pill");
      g_signal_connect (uninstall, "clicked", G_CALLBACK (on_uninstall_clicked), self);
      gtk_box_append (self->action_box, uninstall);

      GtkWidget *reveal = gtk_button_new_from_icon_name ("folder-open-symbolic");
      gtk_widget_set_tooltip_text (reveal, "Show installed files");
      gtk_accessible_update_property (GTK_ACCESSIBLE (reveal), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Show installed files", -1);
      g_signal_connect (reveal, "clicked", G_CALLBACK (on_reveal_clicked), self);
      gtk_box_append (self->action_box, reveal);

      if (gog_game_get_runner_kind (self->game) == GOG_RUNNER_WINE)
        {
          GtkWidget *winecfg = gtk_button_new_with_label ("winecfg");
          g_signal_connect (winecfg, "clicked", G_CALLBACK (on_winecfg_clicked), self);
          gtk_box_append (self->action_box, winecfg);
        }
      return;
    }

  GPtrArray *installers = gog_game_get_installers (self->game);
  if (installers->len == 0)
    {
      GtkWidget *lbl = gtk_label_new ("No installer files found in this folder.");
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (self->action_box, lbl);
      return;
    }

  for (guint i = 0; i < installers->len; i++)
    {
      GogInstallerInfo *info = g_ptr_array_index (installers, i);
      if (info->os == GOG_OS_MAC)
        continue; /* not installable from this app */

      gchar *size_str = text_format_size (info->size);
      gchar *label = g_strdup_printf ("Install (%s, %s, %s)", gog_os_to_string (info->os),
                                       info->language_full ? info->language_full : "?", size_str);
      GtkWidget *btn = gtk_button_new_with_label (label);
      gtk_widget_add_css_class (btn, "suggested-action");
      gtk_widget_add_css_class (btn, "pill");
      g_object_set_data (G_OBJECT (btn), "installer", info);
      g_signal_connect (btn, "clicked", G_CALLBACK (on_install_clicked), self);
      gtk_box_append (self->action_box, btn);
      g_free (label);
      g_free (size_str);
    }
}

static void
build_extras (GogGameDetail *self)
{
  clear_box (self->extras_box);
  GPtrArray *extras = gog_game_get_extras (self->game);
  if (extras->len == 0)
    {
      GtkWidget *lbl = gtk_label_new ("No bonus content downloaded for this game.");
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (self->extras_box, lbl);
      return;
    }

  for (guint i = 0; i < extras->len; i++)
    {
      GogExtraInfo *extra = g_ptr_array_index (extras, i);
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), extra->name);
      gchar *size_str = text_format_size (extra->size);
      gchar *subtitle = g_strdup_printf ("%s · %s", extra->type, size_str);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      GtkWidget *open_btn = gtk_button_new_from_icon_name ("document-open-symbolic");
      gtk_widget_set_valign (open_btn, GTK_ALIGN_CENTER);
      gchar *open_label = g_strdup_printf ("Open %s", extra->name);
      gtk_widget_set_tooltip_text (open_btn, open_label);
      gtk_accessible_update_property (GTK_ACCESSIBLE (open_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      open_label, -1);
      g_free (open_label);
      gtk_widget_add_css_class (open_btn, "flat");
      g_signal_connect_data (open_btn, "clicked", G_CALLBACK (open_extra_file),
                              g_strdup (extra->file_path), extra_path_closure_notify, 0);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), open_btn);
      gtk_box_append (self->extras_box, row);
      g_free (subtitle);
      g_free (size_str);
    }
}

static void
build_dlc (GogGameDetail *self)
{
  clear_box (self->dlc_box);

  guint n = g_list_model_get_n_items (gog_library_get_model (self->library));
  guint found = 0;
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (gog_library_get_model (self->library), i);
      g_object_unref (g); /* the store keeps it alive */
      if (gog_game_get_base_game (g) != self->game)
        continue;
      found++;

      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), gog_game_get_title (g));
      if (gog_game_get_installed (g))
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "Installed");
        }
      else
        {
          GPtrArray *installers = gog_game_get_installers (g);
          if (installers->len > 0)
            {
              GogInstallerInfo *info = g_ptr_array_index (installers, 0);
              GtkWidget *btn = gtk_button_new_with_label ("Install");
              gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
              gtk_widget_add_css_class (btn, "suggested-action");
              g_object_set_data (G_OBJECT (btn), "installer", info);
              /* DLC installs target this game's own record (g), not self->game. */
              g_object_set_data_full (G_OBJECT (btn), "dlc-game", g_object_ref (g), g_object_unref);
              g_signal_connect (btn, "clicked", G_CALLBACK (on_install_clicked), self);
              adw_action_row_add_suffix (ADW_ACTION_ROW (row), btn);
            }
          else
            adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "No installer files found");
        }
      gtk_box_append (self->dlc_box, row);
    }

  if (found == 0)
    {
      GtkWidget *lbl = gtk_label_new ("No downloaded DLC linked to this game was found.");
      gtk_widget_add_css_class (lbl, "dim-label");
      gtk_box_append (self->dlc_box, lbl);
    }
}

static void
refresh (GogGameDetail *self)
{
  if (!self->game)
    return;

  gtk_label_set_label (self->title_label, gog_game_get_title (self->game));

  GString *badges = g_string_new (NULL);
  if (gog_game_get_supports_os (self->game, GOG_OS_WINDOWS))
    g_string_append (badges, "Windows  ");
  if (gog_game_get_supports_os (self->game, GOG_OS_LINUX))
    g_string_append (badges, "Linux  ");
  if (gog_game_get_supports_os (self->game, GOG_OS_MAC))
    g_string_append (badges, "macOS  ");
  gchar *size_str = text_format_size (gog_game_get_total_size (self->game));
  g_string_append (badges, size_str);
  g_free (size_str);
  if (gog_game_get_installed (self->game))
    g_string_append (badges, "  ·  Installed");
  gtk_label_set_label (self->badges_label, badges->str);
  g_string_free (badges, TRUE);

  if (gog_game_get_logo_path (self->game))
    gtk_picture_set_filename (self->logo, gog_game_get_logo_path (self->game));
  else
    gtk_picture_set_filename (self->logo, NULL);

  gtk_label_set_label (self->description_label,
                        gog_game_get_description (self->game) ? gog_game_get_description (self->game)
                                                                : "");

  build_action_row (self);
  build_extras (self);
  build_dlc (self);
}

GogGameDetail *
gog_game_detail_new (GtkWindow *parent_window, GogLibrary *library, GogConfig *config, GogState *state)
{
  GogGameDetail *self = g_object_new (GOG_TYPE_GAME_DETAIL, NULL);
  self->parent_window = parent_window;
  self->library = library;
  self->config = config;
  self->state = state;

  self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (self->toast_overlay), TRUE);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toast_overlay));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  adw_toast_overlay_set_child (self->toast_overlay, scroller);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 760);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start (content, 18);
  gtk_widget_set_margin_end (content, 18);
  gtk_widget_set_margin_top (content, 18);
  gtk_widget_set_margin_bottom (content, 24);
  adw_clamp_set_child (ADW_CLAMP (clamp), content);

  self->logo = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->logo, GTK_CONTENT_FIT_COVER);
  gtk_widget_set_size_request (GTK_WIDGET (self->logo), -1, 200);
  gtk_widget_add_css_class (GTK_WIDGET (self->logo), "card");
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->logo));

  self->title_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->title_label), "gog-detail-title");
  gtk_widget_set_halign (GTK_WIDGET (self->title_label), GTK_ALIGN_START);
  gtk_label_set_wrap (self->title_label, TRUE);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->title_label));

  self->badges_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->badges_label), "dim-label");
  gtk_widget_set_halign (GTK_WIDGET (self->badges_label), GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->badges_label));

  self->action_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->action_box));

  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), stack);
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  gtk_box_append (GTK_BOX (content), switcher);

  self->description_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->description_label, TRUE);
  gtk_label_set_xalign (self->description_label, 0.0);
  gtk_widget_set_halign (GTK_WIDGET (self->description_label), GTK_ALIGN_START);
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (self->description_label),
                                       "about", "About", "help-about-symbolic");

  self->extras_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (self->extras_box),
                                       "extras", "Extras", "folder-download-symbolic");

  self->dlc_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (self->dlc_box),
                                       "dlc", "DLC", "applications-games-symbolic");

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (stack));

  return self;
}

void
gog_game_detail_set_game (GogGameDetail *self, GogGame *game)
{
  g_set_object (&self->game, game);
  refresh (self);
}
