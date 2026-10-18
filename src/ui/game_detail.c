/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "game_detail.h"
#include "install_dialog.h"
#include "../installer_engine.h"
#include "../launch.h"
#include "../runner.h"
#include "dialog_window.h"
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
  GtkWidget *runner_group;
  AdwComboRow *runner_row;
  GPtrArray *runner_choices; /* GogRunnerInfo*, parallel to runner_row's model */
  gulong runner_handler;
  GtkLabel *description_label;
  GtkWidget *serials_group;
  GtkLabel *serials_label;
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
  g_clear_pointer (&self->runner_choices, g_ptr_array_unref);
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
  /* The install window can close because the main window is going away;
   * then this page is being torn down too. */
  if (!gtk_widget_in_destruction (GTK_WIDGET (self)))
    {
      if (success)
        toast (self, "Installed successfully");
      refresh (self);
      emit_changed (self);
    }
  g_object_unref (self); /* taken in on_install_clicked */
}

static void
on_install_clicked (GtkButton *button, gpointer user_data)
{
  GogInstallerInfo *installer = g_object_get_data (G_OBJECT (button), "installer");
  GogGameDetail *self = user_data;
  GogGame *target = g_object_get_data (G_OBJECT (button), "dlc-game");
  if (!target)
    target = self->game;
  gog_show_install_dialog (self->parent_window, target, installer, self->config, self->library,
                            self->state, on_install_done, g_object_ref (self));
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
on_uninstall_response (const char *response, gpointer user_data)
{
  GogGameDetail *self = user_data;
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
  GogAlert *alert =
      is_dlc_record
          ? gog_alert_new ("Uninstall DLC?",
                                  "This DLC was installed into its base game's folder and its "
                                  "files can't be separated from the game's, so they stay on disk. "
                                  "It will only be marked as not installed.")
          : gog_alert_new ("Uninstall Game?",
                                  "This removes the installed game files (and its Wine/Proton "
                                  "prefix, if any) from disk, including any DLC installed into "
                                  "it. The original installer files are not affected.");
  gog_alert_add_response (alert, "cancel", "Cancel", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "uninstall", "Uninstall", ADW_RESPONSE_DESTRUCTIVE);
  gog_alert_set_default_response (alert, "cancel");
  gog_alert_set_close_response (alert, "cancel");
  gog_alert_present (alert, self->parent_window, on_uninstall_response, self);
}

static void
on_open_path_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GogGameDetail *self = user_data;
  GError *error = NULL;
  if (!gtk_file_launcher_launch_finish (GTK_FILE_LAUNCHER (source), res, &error) &&
      !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    {
      gchar *name = g_file_get_basename (gtk_file_launcher_get_file (GTK_FILE_LAUNCHER (source)));
      gchar *msg = g_strdup_printf ("Could not open %s: %s", name, error->message);
      toast (self, msg);
      g_free (msg);
      g_free (name);
    }
  g_clear_error (&error);
  g_object_unref (self);
}

/* Opens a folder in the file manager, or a file in its default app.
 * GtkFileLauncher, not GtkUriLauncher: the desktop portal's OpenURI
 * refuses file:// URIs, so a URI launcher silently did nothing. */
static void
open_path (GogGameDetail *self, const char *path)
{
  GFile *file = g_file_new_for_path (path);
  GtkFileLauncher *launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_launch (launcher, self->parent_window, NULL, on_open_path_finished,
                            g_object_ref (self));
  g_object_unref (launcher);
  g_object_unref (file);
}

static void
on_reveal_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  const char *path = gog_game_get_install_path (self->game);
  if (path)
    open_path (self, path);
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
on_open_extra_clicked (GtkButton *button, gpointer user_data)
{
  open_path (user_data, g_object_get_data (G_OBJECT (button), "gog-path"));
}

static void
on_copy_serials_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  const char *serials = self->game ? gog_game_get_serials (self->game) : NULL;
  if (!serials)
    return;
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), serials);
  toast (self, "Serial keys copied");
}

/* Same-kind runners only: Wine and Proton keep their prefixes in different
 * places, so switching kind would need a fresh prefix (a reinstall). */
static void
build_runner_row (GogGameDetail *self)
{
  GogRunnerKind kind = gog_game_get_runner_kind (self->game);
  gboolean show = gog_game_get_installed (self->game) && !gog_game_get_base_game (self->game) &&
                  (kind == GOG_RUNNER_WINE || kind == GOG_RUNNER_PROTON);
  gtk_widget_set_visible (self->runner_group, show);
  if (!show)
    return;

  const char *current = gog_game_get_runner_path (self->game);
  g_clear_pointer (&self->runner_choices, g_ptr_array_unref);
  self->runner_choices = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_runner_info_free);
  GPtrArray *all = runner_discover_all (self->config);
  guint selected = G_MAXUINT;
  for (guint i = 0; i < all->len; i++)
    {
      GogRunnerInfo *r = g_ptr_array_index (all, i);
      if (r->kind != kind)
        continue;
      if (g_strcmp0 (r->bin_path, current) == 0)
        selected = self->runner_choices->len;
      g_ptr_array_add (self->runner_choices, gog_runner_info_new (r->kind, r->name, r->bin_path));
    }
  g_ptr_array_unref (all);

  gboolean missing = selected == G_MAXUINT;
  GtkStringList *list = gtk_string_list_new (NULL);
  for (guint i = 0; i < self->runner_choices->len; i++)
    gtk_string_list_append (list, ((GogRunnerInfo *) g_ptr_array_index (self->runner_choices, i))->name);
  if (missing)
    {
      /* Keep the recorded runner visible so the user sees what is gone. */
      gchar *name = current ? g_path_get_basename (current) : g_strdup ("None");
      gchar *label = g_strdup_printf ("%s (missing)", name);
      gtk_string_list_append (list, label);
      g_ptr_array_add (self->runner_choices, gog_runner_info_new (kind, name, current));
      selected = self->runner_choices->len - 1;
      g_free (label);
      g_free (name);
    }

  g_signal_handler_block (self->runner_row, self->runner_handler);
  adw_combo_row_set_model (self->runner_row, G_LIST_MODEL (list));
  adw_combo_row_set_selected (self->runner_row, selected);
  g_signal_handler_unblock (self->runner_row, self->runner_handler);
  g_object_unref (list);

  adw_action_row_set_subtitle (ADW_ACTION_ROW (self->runner_row),
                               missing ? "The runner this game used is no longer on this computer. "
                                         "Choose another one."
                                       : (kind == GOG_RUNNER_PROTON ? "Proton version used to run this game"
                                                                    : "Wine used to run this game"));
}

static void
on_runner_selected (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  (void) row;
  (void) pspec;
  GogGameDetail *self = user_data;
  guint idx = adw_combo_row_get_selected (self->runner_row);
  if (!self->game || !self->runner_choices || idx >= self->runner_choices->len)
    return;
  GogRunnerInfo *r = g_ptr_array_index (self->runner_choices, idx);
  const char *old_path = gog_game_get_runner_path (self->game);
  if (g_strcmp0 (r->bin_path, old_path) == 0)
    return;

  /* DLC installed into this game's directory runs through the same prefix,
   * so it follows along. */
  const char *install_path = gog_game_get_install_path (self->game);
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      if (g == self->game ||
          (gog_game_get_base_game (g) == self->game && gog_game_get_installed (g) &&
           g_strcmp0 (gog_game_get_install_path (g), install_path) == 0))
        {
          gog_game_set_runner_path (g, r->bin_path);
          gog_state_update_from_game (self->state, g);
        }
      g_object_unref (g);
    }

  GError *error = NULL;
  if (!gog_state_save (self->state, &error))
    {
      toast (self, error->message);
      g_clear_error (&error);
    }
  else
    {
      gchar *msg = g_strdup_printf ("%s now uses %s", gog_game_get_title (self->game), r->name);
      toast (self, msg);
      g_free (msg);
    }
  build_runner_row (self);
  emit_changed (self);
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
      g_object_set_data_full (G_OBJECT (open_btn), "gog-path", g_strdup (extra->file_path), g_free);
      g_signal_connect (open_btn, "clicked", G_CALLBACK (on_open_extra_clicked), self);
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
              /* Every DLC row has an "Install" button; say which DLC. */
              gchar *btn_label = g_strdup_printf ("Install %s", gog_game_get_title (g));
              gtk_accessible_update_property (GTK_ACCESSIBLE (btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                              btn_label, -1);
              g_free (btn_label);
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
  GStrv installer_langs = gog_game_dup_installer_languages (self->game);
  if (installer_langs[0])
    {
      gchar *joined = g_strjoinv (", ", installer_langs);
      g_string_append_printf (badges, "\nInstallers in %s", joined);
      g_free (joined);
    }
  g_strfreev (installer_langs);
  gtk_label_set_label (self->badges_label, badges->str);
  g_string_free (badges, TRUE);

  if (gog_game_get_logo_path (self->game))
    gtk_picture_set_filename (self->logo, gog_game_get_logo_path (self->game));
  else
    gtk_picture_set_filename (self->logo, NULL);

  const char *serials = gog_game_get_serials (self->game);
  gtk_label_set_label (self->serials_label, serials ? serials : "");
  gtk_widget_set_visible (self->serials_group, serials != NULL);

  gtk_label_set_label (self->description_label,
                        gog_game_get_description (self->game) ? gog_game_get_description (self->game)
                                                                : "");

  build_action_row (self);
  build_runner_row (self);
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
  gtk_label_set_wrap (self->badges_label, TRUE);
  gtk_label_set_xalign (self->badges_label, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (self->badges_label), "dim-label");
  gtk_widget_set_halign (GTK_WIDGET (self->badges_label), GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->badges_label));

  self->action_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->action_box));

  self->runner_group = adw_preferences_group_new ();
  self->runner_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->runner_row), "Runner");
  self->runner_handler = g_signal_connect (self->runner_row, "notify::selected",
                                           G_CALLBACK (on_runner_selected), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->runner_group),
                             GTK_WIDGET (self->runner_row));
  gtk_widget_set_visible (self->runner_group, FALSE);
  gtk_box_append (GTK_BOX (content), self->runner_group);

  /* Serial keys from serials.txt, shown verbatim (the file labels each key
   * itself, e.g. "Doom 3:" followed by the key). Hidden when there are none. */
  self->serials_group = adw_preferences_group_new ();
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (self->serials_group), "Serial Keys");
  GtkWidget *copy_btn = gtk_button_new_from_icon_name ("edit-copy-symbolic");
  gtk_widget_add_css_class (copy_btn, "flat");
  gtk_widget_set_valign (copy_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (copy_btn, "Copy serial keys");
  gtk_accessible_update_property (GTK_ACCESSIBLE (copy_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Copy serial keys", -1);
  g_signal_connect (copy_btn, "clicked", G_CALLBACK (on_copy_serials_clicked), self);
  adw_preferences_group_set_header_suffix (ADW_PREFERENCES_GROUP (self->serials_group), copy_btn);
  self->serials_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_selectable (self->serials_label, TRUE);
  gtk_label_set_wrap (self->serials_label, TRUE);
  gtk_label_set_wrap_mode (self->serials_label, PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign (self->serials_label, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (self->serials_label), "monospace");
  gtk_widget_add_css_class (GTK_WIDGET (self->serials_label), "card");
  gtk_widget_add_css_class (GTK_WIDGET (self->serials_label), "gog-serials");
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->serials_group),
                             GTK_WIDGET (self->serials_label));
  gtk_widget_set_visible (self->serials_group, FALSE);
  gtk_box_append (GTK_BOX (content), self->serials_group);

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
