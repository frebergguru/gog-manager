/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "game_detail.h"
#include "install_dialog.h"
#include "../installer_engine.h"
#include "../launch.h"
#include "../shortcut.h"
#include "../runner.h"
#include "dialog_window.h"
#include "../text_util.h"
#include "../gamesdb.h"
#include "repair_dialog.h"

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
  GtkGrid *info_grid; /* "Developer  id Software" and so on, rebuilt by refresh() */
  GtkBox *action_box;
  GtkBox *tools_box; /* Check Files, under the actions */
  GtkWidget *runner_group;
  AdwComboRow *runner_row;
  GPtrArray *runner_choices; /* GogRunnerInfo*, parallel to runner_row's model */
  gulong runner_handler;
  GtkLabel *description_label;
  GtkLabel *copyright_label;
  GtkBox *messages_box;     /* GOG's messages about the game, above the description */
  GtkBox *links_box;        /* Store Page, Support, Forum */
  GtkBox *requirements_box; /* a group per system and level */
  AdwViewStackPage *requirements_page;
  GtkLabel *changelog_label;
  AdwViewStackPage *changelog_page;
  gulong store_handler;     /* notify::store on game */
  GtkWidget *serials_group;
  GtkLabel *serials_label;
  GtkBox *extras_box;
  GtkBox *dlc_box;
  AdwViewStack *stack;
  AdwViewStackPage *screenshots_page;
  AdwViewStackPage *videos_page;
  GtkFlowBox *screenshots_box; /* thumbnails of shots, filled as they arrive */
  GStrv shots;                 /* url_formats in the gallery; NULL when none */
  gboolean shots_fetching;     /* thumbnails are being fetched for shots */
  GCancellable *shots_cancellable;
  GtkListBox *videos_list;
  gulong gamesdb_handler; /* notify::gamesdb on game */
  AdwToastOverlay *toast_overlay;
  gboolean disposed; /* late async callbacks must not touch widgets */
};

enum
{
  SIG_CHANGED,
  SIG_DOWNLOAD,
  N_SIGNALS,
};

static guint detail_signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GogGameDetail, gog_game_detail, GTK_TYPE_BOX)

static void
gog_game_detail_dispose (GObject *object)
{
  GogGameDetail *self = GOG_GAME_DETAIL (object);
  self->disposed = TRUE;
  /* A newer page may have taken the handler over; leave it alone then. */
  gog_launch_clear_failed_func (self);
  if (self->shots_cancellable)
    g_cancellable_cancel (self->shots_cancellable);
  g_clear_object (&self->shots_cancellable);
  g_clear_pointer (&self->shots, g_strfreev);
  if (self->game)
    {
      g_clear_signal_handler (&self->gamesdb_handler, self->game);
      g_clear_signal_handler (&self->store_handler, self->game);
    }
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
  detail_signals[SIG_DOWNLOAD] =
      g_signal_new (GOG_GAME_DETAIL_SIGNAL_DOWNLOAD, G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gog_game_detail_init (GogGameDetail *self)
{
  gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
}

static void refresh (GogGameDetail *self);

/* The page's widest content, and the banner's width at it (less the
 * margins). */
#define PAGE_WIDTH 760
#define BANNER_WIDTH (PAGE_WIDTH - 2 * 18)

static void
emit_changed (GogGameDetail *self)
{
  g_signal_emit (self, detail_signals[SIG_CHANGED], 0);
}

/* refresh() rebuilds the action buttons, so the one that had keyboard
 * focus is gone; give it to the first of the new ones. */
static void
focus_first_action (GogGameDetail *self)
{
  GtkWidget *first = gtk_widget_get_first_child (GTK_WIDGET (self->action_box));
  if (first && gtk_widget_get_mapped (first))
    gtk_widget_grab_focus (first);
}

static void
toast (GogGameDetail *self, const char *msg)
{
  adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (msg));
}

static void
on_launch_failed (const char *message, gpointer user_data)
{
  toast (user_data, message);
}

static void
on_install_done (GogGame *game, gboolean success, gpointer user_data)
{
  GogGameDetail *self = user_data;
  (void) game;
  /* The install window can close because the main window is going away;
   * then this page is being torn down too. */
  if (!self->disposed && !gtk_widget_in_destruction (GTK_WIDGET (self)))
    {
      if (success)
        toast (self, "Installed successfully");
      refresh (self);
      emit_changed (self);
      focus_first_action (self);
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
  /* Its menu entry would start a game that is gone. */
  if (!is_dlc_record)
    {
      GError *shortcut_err = NULL;
      if (!gog_shortcut_remove (self->game, &shortcut_err))
        {
          g_warning ("%s", shortcut_err->message);
          g_clear_error (&shortcut_err);
        }
    }

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
      /* The files are gone either way; the page must say so. */
      toast (self, save_err->message);
      g_clear_error (&save_err);
    }
  else
    toast (self, is_dlc_record ? "DLC marked as not installed" : "Uninstalled");
  refresh (self);
  emit_changed (self);
  focus_first_action (self);
}

/* Label and tooltip for the menu-entry button; changed in place so it keeps
 * keyboard focus. */
static void
set_shortcut_button (GtkWidget *button, gboolean has_entry)
{
  gtk_button_set_label (GTK_BUTTON (button), has_entry ? "Remove from Menu" : "Add to Menu");
  gtk_widget_set_tooltip_text (button, has_entry ? "Remove the game from the Applications menu"
                                                 : "Put the game in the Applications menu; it "
                                                   "starts without gog-manager");
}

static void
on_shortcut_clicked (GtkButton *button, gpointer user_data)
{
  GogGameDetail *self = user_data;
  GError *error = NULL;
  gboolean remove = gog_shortcut_exists (self->game);
  if (remove ? gog_shortcut_remove (self->game, &error) : gog_shortcut_create (self->game, &error))
    toast (self, remove ? "Removed from the Applications menu" : "Added to the Applications menu");
  else
    {
      toast (self, error->message);
      g_clear_error (&error);
    }
  set_shortcut_button (GTK_WIDGET (button), gog_shortcut_exists (self->game));
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
      !self->disposed &&
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

/* play.sh bakes in the runner path, so it follows a runner change. */
static void
refresh_play_script (GogGame *game)
{
  GError *error = NULL;
  if (!gog_write_play_script (game, &error))
    {
      g_warning ("Could not write play.sh: %s", error->message);
      g_clear_error (&error);
    }
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
          if (g == self->game)
            refresh_play_script (g);
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

/* For people; gog_os_to_string() is the id kept in files. */
static const char *
os_label (GogOs os)
{
  switch (os)
    {
    case GOG_OS_WINDOWS: return "Windows";
    case GOG_OS_LINUX: return "Linux";
    case GOG_OS_MAC: return "macOS";
    default: return "Unknown system";
    }
}

static void
on_check_files_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  gchar *library_dir = gog_config_effective_library_path (self->config);
  gog_show_repair_dialog (self->parent_window, library_dir, self->game);
  g_free (library_dir);
}

/* Every downloaded game's files can be checked against GOG's. */
static void
build_tools_row (GogGameDetail *self)
{
  clear_box (self->tools_box);
  gboolean show = gog_game_get_downloaded (self->game) && gog_game_get_folder_path (self->game) &&
                  !gog_game_get_base_game (self->game);
  gtk_widget_set_visible (GTK_WIDGET (self->tools_box), show);
  if (!show)
    return;
  GtkWidget *check = gtk_button_new_with_label ("Check Files…");
  gtk_widget_set_tooltip_text (check, "Compare the downloaded files with GOG's checksums and "
                                      "download damaged parts again (lgogdownloader --repair)");
  g_signal_connect (check, "clicked", G_CALLBACK (on_check_files_clicked), self);
  gtk_box_append (self->tools_box, check);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogGameDetail *self = user_data;
  g_signal_emit (self, detail_signals[SIG_DOWNLOAD], 0, gog_game_get_slug (self->game));
}

static void
build_action_row (GogGameDetail *self)
{
  clear_box (self->action_box);

  /* Owned on GOG, no files here yet: the one thing to do is fetch them. */
  if (!gog_game_get_downloaded (self->game))
    {
      GtkWidget *download = gtk_button_new_with_label ("Download…");
      gtk_widget_add_css_class (download, "suggested-action");
      gtk_widget_add_css_class (download, "pill");
      gtk_widget_set_tooltip_text (download, "Choose its languages, DLC and extras, and "
                                             "download it from GOG with lgogdownloader");
      g_signal_connect (download, "clicked", G_CALLBACK (on_download_clicked), self);
      gtk_box_append (self->action_box, download);
      return;
    }

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

      /* DLC starts through its game, which has the entry. */
      if (!gog_game_get_base_game (self->game))
        {
          GtkWidget *shortcut = gtk_button_new ();
          gtk_widget_add_css_class (shortcut, "pill");
          set_shortcut_button (shortcut, gog_shortcut_exists (self->game));
          g_signal_connect (shortcut, "clicked", G_CALLBACK (on_shortcut_clicked), self);
          gtk_box_append (self->action_box, shortcut);
        }

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
      gchar *lang = text_language_label (info->language_full ? info->language_full : "?");
      gchar *label = g_strdup_printf ("Install (%s, %s, %s)", os_label (info->os), lang,
                                       size_str);
      g_free (lang);
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
      /* GOG's names and types are plain text: "guides & reference". */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
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
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
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

static gboolean
strv_equal (GStrv a, GStrv b)
{
  return a && b && g_strv_equal ((const char *const *) a, (const char *const *) b);
}

/* A row of the details table; nothing when value is NULL or empty. */
static void
add_info (GtkGrid *grid, int *row, const char *what, const char *value)
{
  if (!value || !*value)
    return;
  GtkWidget *key = gtk_label_new (what);
  gtk_label_set_xalign (GTK_LABEL (key), 0.0);
  gtk_label_set_yalign (GTK_LABEL (key), 0.0);
  gtk_widget_add_css_class (key, "dim-label");
  GtkWidget *val = gtk_label_new (value);
  gtk_label_set_xalign (GTK_LABEL (val), 0.0);
  gtk_label_set_wrap (GTK_LABEL (val), TRUE);
  gtk_label_set_selectable (GTK_LABEL (val), FALSE);
  gtk_widget_set_hexpand (val, TRUE);
  gtk_grid_attach (grid, key, 0, *row, 1, 1);
  gtk_grid_attach (grid, val, 1, *row, 1, 1);
  (*row)++;
}

static void
add_info_list (GtkGrid *grid, int *row, const char *what, GStrv list)
{
  if (!list || !list[0])
    return;
  gchar *joined = g_strjoinv (", ", list);
  add_info (grid, row, what, joined);
  g_free (joined);
}

static gchar *
format_date (gint64 unix_time)
{
  GDateTime *dt = unix_time > 0 ? g_date_time_new_from_unix_utc (unix_time) : NULL;
  gchar *date = dt ? g_date_time_format (dt, "%Y-%m-%d") : NULL;
  if (dt)
    g_date_time_unref (dt);
  return date;
}

/* ---------------- screenshots ---------------- */

/* A screenshot full size in a window of its own, with Previous and Next
 * (also the arrow keys). Fetched when shown; the thumbnail stands in
 * meanwhile. */
typedef struct
{
  GtkWindow *window;
  GStrv shots;
  guint index;
  gchar *title; /* the game's */
  GtkPicture *picture;
  GtkLabel *position;
  GtkWidget *prev;
  GtkWidget *next;
  GCancellable *cancellable;
} Viewer;

static void
viewer_free (Viewer *v)
{
  g_cancellable_cancel (v->cancellable);
  g_object_unref (v->cancellable);
  g_strfreev (v->shots);
  g_free (v->title);
  g_free (v);
}

static void viewer_show (Viewer *v, gboolean announce);

static void
full_shot_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  (void) source;
  const char *formats[] = { task_data, NULL };
  GError *error = NULL;
  if (gog_gamesdb_fetch_screenshots (formats, TRUE, cancellable, NULL, NULL, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
on_full_shot_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  /* The task keeps the window, and so its viewer, alive until here. */
  Viewer *v = g_object_get_data (source, "viewer");
  g_task_propagate_boolean (G_TASK (res), NULL);
  if (v && !g_cancellable_is_cancelled (v->cancellable))
    viewer_show (v, FALSE);
}

static void
viewer_show (Viewer *v, gboolean announce)
{
  guint n = g_strv_length (v->shots);
  const char *format = v->shots[v->index];
  gchar *full = gog_gamesdb_screenshot_path (format, TRUE);
  gboolean have_full = full && g_file_test (full, G_FILE_TEST_IS_REGULAR);
  gchar *thumb = have_full ? NULL : gog_gamesdb_screenshot_path (format, FALSE);
  const char *path = have_full ? full : thumb;
  gtk_picture_set_filename (v->picture,
                            path && g_file_test (path, G_FILE_TEST_IS_REGULAR) ? path : NULL);

  gchar *text = g_strdup_printf ("%u of %u", v->index + 1, n);
  gtk_label_set_label (v->position, text);
  gchar *alt = g_strdup_printf ("Screenshot %u of %u of %s%s", v->index + 1, n, v->title,
                                have_full ? "" : ", loading the full size");
  gtk_picture_set_alternative_text (v->picture, alt);
  gtk_accessible_update_property (GTK_ACCESSIBLE (v->picture), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  alt, -1);
  if (announce)
    gog_announce (GTK_WIDGET (v->window), alt);
  gtk_widget_set_sensitive (v->prev, v->index > 0);
  gtk_widget_set_sensitive (v->next, v->index + 1 < n);

  if (!have_full && full)
    {
      GTask *task = g_task_new (v->window, v->cancellable, on_full_shot_done, NULL);
      g_task_set_task_data (task, g_strdup (format), g_free);
      g_task_run_in_thread (task, full_shot_thread);
      g_object_unref (task);
    }
  g_free (alt);
  g_free (text);
  g_free (thumb);
  g_free (full);
}

static void
viewer_step (Viewer *v, int step)
{
  guint n = g_strv_length (v->shots);
  if ((step < 0 && v->index == 0) || (step > 0 && v->index + 1 >= n))
    return;
  v->index += step;
  viewer_show (v, TRUE);
}

static void
on_viewer_prev (GtkButton *button, gpointer user_data)
{
  (void) button;
  viewer_step (user_data, -1);
}

static void
on_viewer_next (GtkButton *button, gpointer user_data)
{
  (void) button;
  viewer_step (user_data, 1);
}

static gboolean
on_viewer_key (GtkWidget *widget, GVariant *args, gpointer user_data)
{
  (void) args;
  Viewer *v = g_object_get_data (G_OBJECT (widget), "viewer");
  viewer_step (v, GPOINTER_TO_INT (user_data));
  return TRUE;
}

static void
on_viewer_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  Viewer *v = user_data;
  /* A full size still on its way is not wanted any more. */
  g_cancellable_cancel (v->cancellable);
}

static void
show_viewer (GogGameDetail *self, guint index)
{
  Viewer *v = g_new0 (Viewer, 1);
  v->shots = g_strdupv (self->shots);
  v->index = index;
  v->title = g_strdup (gog_game_get_title (self->game));
  v->cancellable = g_cancellable_new ();
  gchar *title = g_strdup_printf ("Screenshots: %s", v->title);
  v->window = gog_dialog_window_new (self->parent_window, title, 1000, 640, 360, 260);
  g_free (title);
  g_object_set_data_full (G_OBJECT (v->window), "viewer", v, (GDestroyNotify) viewer_free);
  g_signal_connect (v->window, "destroy", G_CALLBACK (on_viewer_destroy), v);

  GtkWidget *view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  v->prev = gtk_button_new_from_icon_name ("go-previous-symbolic");
  v->next = gtk_button_new_from_icon_name ("go-next-symbolic");
  gtk_widget_set_tooltip_text (v->prev, "Previous screenshot (Left)");
  gtk_widget_set_tooltip_text (v->next, "Next screenshot (Right)");
  gtk_accessible_update_property (GTK_ACCESSIBLE (v->prev), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Previous screenshot", -1);
  gtk_accessible_update_property (GTK_ACCESSIBLE (v->next), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Next screenshot", -1);
  g_signal_connect (v->prev, "clicked", G_CALLBACK (on_viewer_prev), v);
  g_signal_connect (v->next, "clicked", G_CALLBACK (on_viewer_next), v);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), v->prev);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), v->next);
  v->position = GTK_LABEL (gtk_label_new (NULL));
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (v->position));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), header);

  v->picture = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (v->picture, GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_vexpand (GTK_WIDGET (v->picture), TRUE);
  gtk_widget_set_hexpand (GTK_WIDGET (v->picture), TRUE);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), GTK_WIDGET (v->picture));
  adw_window_set_content (ADW_WINDOW (v->window), view);

  GtkEventController *keys = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (
      GTK_SHORTCUT_CONTROLLER (keys),
      gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Left, 0),
                        gtk_callback_action_new (on_viewer_key, GINT_TO_POINTER (-1), NULL)));
  gtk_shortcut_controller_add_shortcut (
      GTK_SHORTCUT_CONTROLLER (keys),
      gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Right, 0),
                        gtk_callback_action_new (on_viewer_key, GINT_TO_POINTER (1), NULL)));
  gtk_widget_add_controller (GTK_WIDGET (v->window), keys);

  viewer_show (v, FALSE);
  gtk_window_present (v->window);
  gtk_widget_grab_focus (v->next);
}

/* Shows the thumbnails that are on disk now in the gallery's empty
 * places. */
static void
update_shot_pictures (GogGameDetail *self)
{
  guint i = 0;
  for (GtkWidget *c = gtk_widget_get_first_child (GTK_WIDGET (self->screenshots_box));
       c && self->shots && self->shots[i]; c = gtk_widget_get_next_sibling (c), i++)
    {
      GtkPicture *picture = GTK_PICTURE (gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (c)));
      if (gtk_picture_get_paintable (picture))
        continue;
      gchar *path = gog_gamesdb_screenshot_path (self->shots[i], FALSE);
      if (path && g_file_test (path, G_FILE_TEST_IS_REGULAR))
        gtk_picture_set_filename (picture, path);
      g_free (path);
    }
}

static gboolean
shots_progress_idle (gpointer data)
{
  GogGameDetail *self = data;
  if (!self->disposed)
    update_shot_pictures (self);
  return G_SOURCE_REMOVE;
}

static void
on_shots_progress (guint done, guint total, gboolean images, gpointer user_data)
{
  (void) done;
  (void) total;
  (void) images;
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, shots_progress_idle, g_object_ref (user_data),
                   g_object_unref);
}

static void
shots_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GError *error = NULL;
  if (gog_gamesdb_fetch_screenshots ((const char *const *) task_data, FALSE, cancellable,
                                     on_shots_progress, source, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
on_shots_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  GogGameDetail *self = GOG_GAME_DETAIL (source);
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (res), &error);
  GCancellable *cancellable = g_task_get_cancellable (G_TASK (res));
  /* A later gallery (another game) has its own fetch. */
  if (cancellable == self->shots_cancellable && !self->disposed)
    {
      self->shots_fetching = FALSE;
      update_shot_pictures (self);
      if (!ok && !g_cancellable_is_cancelled (cancellable))
        toast (self, "Could not fetch the screenshots from GOG");
    }
  g_clear_error (&error);
}

/* Fetches the gallery's missing thumbnails, once it's on screen. */
static void
fetch_shots (GogGameDetail *self)
{
  if (!self->shots || self->shots_fetching ||
      g_strcmp0 (adw_view_stack_get_visible_child_name (self->stack), "screenshots") != 0)
    return;
  self->shots_fetching = TRUE;
  GTask *task = g_task_new (self, self->shots_cancellable, on_shots_done, NULL);
  g_task_set_task_data (task, g_strdupv (self->shots), (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, shots_thread);
  g_object_unref (task);
}

static void
on_stack_child_changed (GObject *stack, GParamSpec *pspec, gpointer user_data)
{
  (void) stack;
  (void) pspec;
  fetch_shots (user_data);
}

static void
on_shot_activated (GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data)
{
  (void) box;
  GogGameDetail *self = user_data;
  if (self->shots)
    show_viewer (self, gtk_flow_box_child_get_index (child));
}

/* The gallery, made again only when the game's screenshots change. */
static void
build_screenshots (GogGameDetail *self, const GogGamesdbInfo *db)
{
  /* The games database's, else the product json's. */
  GStrv shots = db && db->screenshots && db->screenshots[0] ? db->screenshots : NULL;
  GStrv product = gog_game_get_product_info (self->game)->screenshots;
  if (!shots && product[0])
    shots = product;
  gboolean same = (!shots && !self->shots) ||
                  (shots && self->shots &&
                   g_strv_equal ((const char *const *) shots, (const char *const *) self->shots));
  if (!same)
    {
      if (self->shots_cancellable)
        g_cancellable_cancel (self->shots_cancellable);
      g_clear_object (&self->shots_cancellable);
      self->shots_cancellable = g_cancellable_new ();
      self->shots_fetching = FALSE;
      g_strfreev (self->shots);
      self->shots = g_strdupv (shots);
      GtkWidget *old;
      while ((old = gtk_widget_get_first_child (GTK_WIDGET (self->screenshots_box))))
        gtk_flow_box_remove (self->screenshots_box, old);
      guint n = shots ? g_strv_length (shots) : 0;
      for (guint i = 0; i < n; i++)
        {
          GtkWidget *picture = gtk_picture_new ();
          gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_COVER);
          /* The least it may shrink to; it is as tall as its column is
           * wide makes it, once it's there. */
          gtk_widget_set_size_request (picture, 160, 90);
          gtk_widget_add_css_class (picture, "card");
          gchar *label = g_strdup_printf ("Screenshot %u of %u", i + 1, n);
          gtk_picture_set_alternative_text (GTK_PICTURE (picture), label);
          GtkWidget *child = gtk_flow_box_child_new ();
          gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (child), picture);
          gtk_widget_set_tooltip_text (child, "Show it larger");
          gtk_accessible_update_property (GTK_ACCESSIBLE (child), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          label, GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                          "Press Enter to show it larger", -1);
          g_free (label);
          gtk_flow_box_append (self->screenshots_box, child);
        }
    }
  update_shot_pictures (self);
  adw_view_stack_page_set_visible (self->screenshots_page, self->shots != NULL);
  fetch_shots (self);
}

/* ---------------- videos ---------------- */

static void
on_uri_launched (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GogGameDetail *self = user_data;
  GError *error = NULL;
  if (!gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (source), res, &error) &&
      !self->disposed && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    {
      gchar *msg = g_strdup_printf ("Could not open %s: %s",
                                    gtk_uri_launcher_get_uri (GTK_URI_LAUNCHER (source)),
                                    error->message);
      toast (self, msg);
      g_free (msg);
    }
  g_clear_error (&error);
  g_object_unref (self);
}

/* Opens a web page in the browser. */
static void
open_uri (GogGameDetail *self, const char *uri)
{
  GtkUriLauncher *launcher = gtk_uri_launcher_new (uri);
  gtk_uri_launcher_launch (launcher, self->parent_window, NULL, on_uri_launched,
                           g_object_ref (self));
  g_object_unref (launcher);
}

static void
on_video_activated (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
  (void) list;
  GogGameDetail *self = user_data;
  const char *id = g_object_get_data (G_OBJECT (row), "video-id");
  if (!id)
    return;
  gchar *uri = g_strdup_printf ("https://www.youtube.com/watch?v=%s", id);
  open_uri (self, uri);
  g_free (uri);
}

/* The game's videos, as links to YouTube: the app plays no video itself. */
static void
build_videos (GogGameDetail *self, const GogGamesdbInfo *db)
{
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child (GTK_WIDGET (self->videos_list))))
    gtk_list_box_remove (self->videos_list, old);
  /* The games database's (which has names), else the product json's. */
  GStrv ids = db && db->video_ids && db->video_ids[0] ? db->video_ids : NULL;
  GStrv names = ids ? db->video_names : NULL;
  if (!ids)
    ids = gog_game_get_product_info (self->game)->video_ids;
  guint n = g_strv_length (ids);
  for (guint i = 0; i < n; i++)
    {
      GtkWidget *row = adw_action_row_new ();
      /* GOG calls most of them "Trailer": the ones that share a name are
       * numbered, so they can be told apart also by ear. */
      const char *name = names && *names[i] ? names[i] : "Video";
      guint same = 0, before = 0;
      for (guint j = 0; j < n; j++)
        {
          const char *other = names && *names[j] ? names[j] : "Video";
          if (g_strcmp0 (other, name) == 0)
            {
              same++;
              before += j < i;
            }
        }
      gchar *title = same > 1 ? g_strdup_printf ("%s %u", name, before + 1) : g_strdup (name);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), "Opens on YouTube in your web browser");
      adw_action_row_add_prefix (ADW_ACTION_ROW (row),
                                 gtk_image_new_from_icon_name ("media-playback-start-symbolic"));
      adw_action_row_add_suffix (ADW_ACTION_ROW (row),
                                 gtk_image_new_from_icon_name ("go-next-symbolic"));
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
      g_object_set_data_full (G_OBJECT (row), "video-id", g_strdup (ids[i]), g_free);
      gtk_list_box_append (self->videos_list, row);
      g_free (title);
    }
  adw_view_stack_page_set_visible (self->videos_page, n > 0);
}

/* ---------------- requirements, changelog, links ---------------- */

/* A group per system and level ("Windows: Minimum"), a row per part. */
static void
build_requirements (GogGameDetail *self, const GogStoreInfo *store)
{
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child (GTK_WIDGET (self->requirements_box))))
    gtk_box_remove (self->requirements_box, old);
  guint n = store ? store->requirements->len : 0;
  for (guint i = 0; i < n; i++)
    {
      GogRequirements *r = g_ptr_array_index (store->requirements, i);
      GtkWidget *group = adw_preferences_group_new ();
      gchar *title = g_strdup_printf ("%s: %s", r->os, r->level);
      adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group), title);
      g_free (title);
      for (guint k = 0; r->names[k]; k++)
        {
          GtkWidget *row = adw_action_row_new ();
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), r->names[k]);
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), r->values[k]);
          /* The values are what matters here; they may be long. */
          adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 0);
          adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
          gtk_widget_add_css_class (row, "property");
          adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
        }
      gtk_box_append (self->requirements_box, group);
    }
  adw_view_stack_page_set_visible (self->requirements_page, n > 0);
}

static void
on_link_clicked (GtkButton *button, gpointer user_data)
{
  const char *uri = g_object_get_data (G_OBJECT (button), "uri");
  if (uri)
    open_uri (user_data, uri);
}

static void
add_link (GogGameDetail *self, const char *label, const char *what, const char *uri)
{
  if (!uri)
    return;
  GtkWidget *button = gtk_button_new_with_label (label);
  gchar *tip = g_strdup_printf ("Open the game's %s on gog.com in your web browser", what);
  gtk_widget_set_tooltip_text (button, tip);
  g_free (tip);
  g_object_set_data_full (G_OBJECT (button), "uri", g_strdup (uri), g_free);
  g_signal_connect (button, "clicked", G_CALLBACK (on_link_clicked), self);
  gtk_box_append (self->links_box, button);
}

/* Markup made from GOG's HTML; should it ever come out invalid, its text
 * instead of a label left showing the previous game's. */
static void
set_markup_or_text (GtkLabel *label, const char *markup)
{
  gchar *text = NULL;
  if (pango_parse_markup (markup, -1, 0, NULL, &text, NULL, NULL))
    gtk_label_set_markup (label, markup);
  else
    {
      gchar *plain = text_strip_html (markup);
      gtk_label_set_text (label, plain);
      g_free (plain);
    }
  g_free (text);
}

/* The links, the changelog and the copyright line. */
static void
build_about_extras (GogGameDetail *self, const GogStoreInfo *store)
{
  const GogProductInfo *product = gog_game_get_product_info (self->game);
  GtkWidget *old;
  while ((old = gtk_widget_get_first_child (GTK_WIDGET (self->links_box))))
    gtk_box_remove (self->links_box, old);
  add_link (self, "Store Page", "store page", product->store_url);
  add_link (self, "Support", "support page", product->support_url);
  add_link (self, "Forum", "forum", product->forum_url);
  gtk_widget_set_visible (GTK_WIDGET (self->links_box),
                          gtk_widget_get_first_child (GTK_WIDGET (self->links_box)) != NULL);

  while ((old = gtk_widget_get_first_child (GTK_WIDGET (self->messages_box))))
    gtk_box_remove (self->messages_box, old);
  for (GStrv m = product->messages; *m; m++)
    {
      if (m == product->messages)
        {
          GtkWidget *heading = gtk_label_new ("From GOG");
          gtk_widget_add_css_class (heading, "heading");
          gtk_label_set_xalign (GTK_LABEL (heading), 0.0);
          gtk_box_append (self->messages_box, heading);
        }
      GtkWidget *label = gtk_label_new (NULL);
      set_markup_or_text (GTK_LABEL (label), *m);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_selectable (GTK_LABEL (label), TRUE);
      gtk_widget_add_css_class (label, "card");
      gtk_widget_add_css_class (label, "gog-message");
      gtk_box_append (self->messages_box, label);
    }
  gtk_widget_set_visible (GTK_WIDGET (self->messages_box), product->messages[0] != NULL);

  const char *copyright = store ? store->copyright : NULL;
  gtk_label_set_label (self->copyright_label, copyright ? copyright : "");
  gtk_widget_set_visible (GTK_WIDGET (self->copyright_label), copyright != NULL);

  set_markup_or_text (self->changelog_label, product->changelog ? product->changelog : "");
  adw_view_stack_page_set_visible (self->changelog_page, product->changelog != NULL);
}

/* What refresh() shows that GOG's details can change: refreshed alone when
 * they arrive while the page is open, so the buttons keep focus. */
static void
refresh_details (GogGameDetail *self)
{

  gtk_label_set_label (self->title_label, gog_game_get_title (self->game));

  /* "Windows · Linux · 4,6 GB · Installed" */
  GPtrArray *badges = g_ptr_array_new_with_free_func (g_free);
  const GogOs systems[] = { GOG_OS_WINDOWS, GOG_OS_LINUX, GOG_OS_MAC };
  for (guint i = 0; i < G_N_ELEMENTS (systems); i++)
    if (gog_game_get_supports_os (self->game, systems[i]))
      g_ptr_array_add (badges, g_strdup (os_label (systems[i])));
  if (gog_game_get_downloaded (self->game))
    g_ptr_array_add (badges, text_format_size (gog_game_get_total_size (self->game)));
  else
    g_ptr_array_add (badges, g_strdup ("Not downloaded"));
  if (gog_game_get_installed (self->game))
    g_ptr_array_add (badges, g_strdup ("Installed"));
  /* Early access: GOG still works on it. */
  const GogStoreInfo *dev_store = gog_game_get_store_info (self->game);
  if (gog_game_get_product_info (self->game)->in_development ||
      (dev_store && dev_store->in_development))
    g_ptr_array_add (badges, g_strdup ("In development"));
  g_ptr_array_add (badges, NULL);
  gchar *badge_text = g_strjoinv (" · ", (gchar **) badges->pdata);
  gtk_label_set_label (self->badges_label, badge_text);
  g_free (badge_text);
  g_ptr_array_unref (badges);

  /* The details as a two-column table: easier to scan than a block of
   * lines. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->info_grid))))
    gtk_grid_remove (self->info_grid, child);
  int row = 0;

  GStrv installer_langs = gog_game_dup_installer_languages (self->game);
  for (gchar **l = installer_langs; *l; l++)
    {
      gchar *label = text_language_label (*l);
      g_free (*l);
      *l = label;
    }
  add_info_list (self->info_grid, &row, "Installers in", installer_langs);
  g_strfreev (installer_langs);
  gchar *released = format_date (gog_game_get_release_date (self->game));
  add_info (self->info_grid, &row, "Released", released);
  const GogGamesdbInfo *db = gog_game_get_gamesdb (self->game);
  /* The game's own first release, when GOG's came later. */
  gchar *first = db ? format_date (db->first_release) : NULL;
  if (g_strcmp0 (first, released) != 0)
    add_info (self->info_grid, &row, "First released", first);
  g_free (first);
  g_free (released);
  add_info_list (self->info_grid, &row, "Categories", (GStrv) gog_game_get_categories (self->game));
  if (db)
    {
      add_info_list (self->info_grid, &row, "Genres", db->genres);
      add_info_list (self->info_grid, &row, "Developer", db->developers);
      if (!strv_equal (db->publishers, db->developers))
        add_info_list (self->info_grid, &row, "Publisher", db->publishers);
      add_info (self->info_grid, &row, "Series", db->series);
      add_info_list (self->info_grid, &row, "Themes", db->themes);
      add_info_list (self->info_grid, &row, "Modes", db->game_modes);
      if (db->rating > 0)
        {
          gchar *rating = g_strdup_printf ("%.0f out of 100", db->rating);
          add_info (self->info_grid, &row, "Critics' rating", rating);
          g_free (rating);
        }
    }
  const GogStoreInfo *store = gog_game_get_store_info (self->game);
  if (store)
    {
      add_info_list (self->info_grid, &row, "Features", store->features);
      add_info_list (self->info_grid, &row, "Tags", store->tags);
      add_info_list (self->info_grid, &row, "Audio in", store->audio_languages);
      add_info_list (self->info_grid, &row, "Text in", store->text_languages);
      if (store->age_ratings[0])
        {
          gchar *ratings = g_strjoinv ("\n", store->age_ratings);
          add_info (self->info_grid, &row, "Age ratings", ratings);
          g_free (ratings);
        }
      add_info_list (self->info_grid, &row, "Runs in", store->technologies);
      add_info_list (self->info_grid, &row, "Editions", store->editions);
    }
  gtk_widget_set_visible (GTK_WIDGET (self->info_grid), row > 0);

  /* GOG's background art when it's there, else lgogdownloader's logo. */
  const char *banner = db && db->background_path ? db->background_path
                                                  : gog_game_get_logo_path (self->game);
  gtk_picture_set_filename (self->logo, banner);
  GdkPaintable *art = gtk_picture_get_paintable (self->logo);
  double ratio = art ? gdk_paintable_get_intrinsic_aspect_ratio (art) : 0;
  /* As tall as the art is at the page's full width, so it shows whole
   * there; a narrower window trims its sides. */
  if (ratio > 0)
    gtk_widget_set_size_request (GTK_WIDGET (self->logo), -1,
                                 (int) CLAMP (BANNER_WIDTH / ratio, 120, 360));
  gtk_widget_set_visible (GTK_WIDGET (self->logo), ratio > 0);

  const char *serials = gog_game_get_serials (self->game);
  gtk_label_set_label (self->serials_label, serials ? serials : "");
  gtk_widget_set_visible (self->serials_group, serials != NULL);

  /* GOG's full description, else its short one, else the games
   * database's summary. */
  const char *description = gog_game_get_description (self->game);
  if (!description || !*description)
    description = gog_game_get_product_info (self->game)->lead;
  if ((!description || !*description) && db)
    description = db->summary;
  /* GOG's text for some is only placeholders (DOOM I Enhanced), and
   * gamesdb may not know them: say so rather than show an empty tab. */
  gboolean none = !description || !*description;
  gtk_label_set_label (self->description_label,
                       none ? "GOG gives no description for this game." : description);
  if (none)
    gtk_widget_add_css_class (GTK_WIDGET (self->description_label), "dim-label");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self->description_label), "dim-label");

  build_screenshots (self, db);
  build_videos (self, db);
  build_requirements (self, store);
  build_about_extras (self, store);
}

static void
refresh (GogGameDetail *self)
{
  if (!self->game)
    return;

  refresh_details (self);
  build_action_row (self);
  build_tools_row (self);
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
  /* A launch that sets up its prefix first can fail after Play returned. */
  gog_launch_set_failed_func (on_launch_failed, self);
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->toast_overlay));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  adw_toast_overlay_set_child (self->toast_overlay, scroller);

  GtkWidget *clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), PAGE_WIDTH);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), clamp);

  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start (content, 18);
  gtk_widget_set_margin_end (content, 18);
  gtk_widget_set_margin_top (content, 18);
  gtk_widget_set_margin_bottom (content, 24);
  adw_clamp_set_child (ADW_CLAMP (clamp), content);

  /* Its height follows the art's shape; see refresh(). */
  self->logo = GTK_PICTURE (gtk_picture_new ());
  gtk_picture_set_content_fit (self->logo, GTK_CONTENT_FIT_COVER);
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

  self->info_grid = GTK_GRID (gtk_grid_new ());
  gtk_grid_set_column_spacing (self->info_grid, 18);
  gtk_grid_set_row_spacing (self->info_grid, 4);
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->info_grid));

  self->action_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->action_box));
  self->tools_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_box_append (GTK_BOX (content), GTK_WIDGET (self->tools_box));

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
  self->stack = stack;
  /* Each tab as tall as its own content: a long screenshot gallery mustn't
   * leave a short description under a screen of empty space. */
  adw_view_stack_set_vhomogeneous (stack, FALSE);
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), stack);
  /* Icons above the labels: up to seven tabs must fit the page's width
   * without cutting their names short. */
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_NARROW);
  gtk_box_append (GTK_BOX (content), switcher);

  self->description_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->description_label, TRUE);
  gtk_label_set_xalign (self->description_label, 0.0);
  gtk_label_set_yalign (self->description_label, 0.0);
  /* The page's full width, top aligned: with GTK_ALIGN_START it took the
   * narrowest width that fit the tallest tab's height, a thin column under
   * a gap. */
  gtk_widget_set_valign (GTK_WIDGET (self->description_label), GTK_ALIGN_START);
  GtkWidget *about = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (about, GTK_ALIGN_START);
  self->messages_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 6));
  gtk_widget_set_visible (GTK_WIDGET (self->messages_box), FALSE);
  gtk_box_append (GTK_BOX (about), GTK_WIDGET (self->messages_box));
  gtk_box_append (GTK_BOX (about), GTK_WIDGET (self->description_label));
  self->links_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8));
  gtk_box_append (GTK_BOX (about), GTK_WIDGET (self->links_box));
  self->copyright_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->copyright_label, TRUE);
  gtk_label_set_xalign (self->copyright_label, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (self->copyright_label), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->copyright_label), "caption");
  gtk_box_append (GTK_BOX (about), GTK_WIDGET (self->copyright_label));
  adw_view_stack_add_titled_with_icon (stack, about, "about", "About", "help-about-symbolic");

  self->extras_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (self->extras_box),
                                       "extras", "Extras", "folder-download-symbolic");

  self->dlc_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 4));
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (self->dlc_box),
                                       "dlc", "DLC", "applications-games-symbolic");

  /* Arrow keys move between the thumbnails, Enter shows one larger. */
  self->screenshots_box = GTK_FLOW_BOX (gtk_flow_box_new ());
  gtk_flow_box_set_selection_mode (self->screenshots_box, GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous (self->screenshots_box, TRUE);
  /* The flow box would go by the thumbnails' own width (510) and fit one
   * a line. */
  gtk_flow_box_set_min_children_per_line (self->screenshots_box, 3);
  gtk_flow_box_set_max_children_per_line (self->screenshots_box, 3);
  gtk_flow_box_set_column_spacing (self->screenshots_box, 8);
  gtk_flow_box_set_row_spacing (self->screenshots_box, 8);
  gtk_widget_set_valign (GTK_WIDGET (self->screenshots_box), GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->screenshots_box),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Screenshots", -1);
  g_signal_connect (self->screenshots_box, "child-activated", G_CALLBACK (on_shot_activated), self);
  self->screenshots_page = adw_view_stack_add_titled_with_icon (
      stack, GTK_WIDGET (self->screenshots_box), "screenshots", "Screenshots",
      "image-x-generic-symbolic");
  adw_view_stack_page_set_visible (self->screenshots_page, FALSE);

  self->videos_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->videos_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (self->videos_list), "boxed-list");
  gtk_widget_set_valign (GTK_WIDGET (self->videos_list), GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->videos_list),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Videos", -1);
  g_signal_connect (self->videos_list, "row-activated", G_CALLBACK (on_video_activated), self);
  self->videos_page = adw_view_stack_add_titled_with_icon (
      stack, GTK_WIDGET (self->videos_list), "videos", "Videos", "video-x-generic-symbolic");
  adw_view_stack_page_set_visible (self->videos_page, FALSE);

  self->requirements_box = GTK_BOX (gtk_box_new (GTK_ORIENTATION_VERTICAL, 18));
  gtk_widget_set_valign (GTK_WIDGET (self->requirements_box), GTK_ALIGN_START);
  self->requirements_page = adw_view_stack_add_titled_with_icon (
      stack, GTK_WIDGET (self->requirements_box), "requirements", "Requirements",
      "computer-symbolic");
  adw_view_stack_page_set_visible (self->requirements_page, FALSE);

  self->changelog_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->changelog_label, TRUE);
  gtk_label_set_xalign (self->changelog_label, 0.0);
  gtk_label_set_yalign (self->changelog_label, 0.0);
  gtk_label_set_selectable (self->changelog_label, TRUE);
  gtk_widget_set_valign (GTK_WIDGET (self->changelog_label), GTK_ALIGN_START);
  self->changelog_page = adw_view_stack_add_titled_with_icon (
      stack, GTK_WIDGET (self->changelog_label), "changelog", "Changelog",
      "document-edit-symbolic");
  adw_view_stack_page_set_visible (self->changelog_page, FALSE);

  g_signal_connect (stack, "notify::visible-child", G_CALLBACK (on_stack_child_changed), self);

  gtk_box_append (GTK_BOX (content), GTK_WIDGET (stack));

  return self;
}

static void
on_game_gamesdb_changed (GObject *game, GParamSpec *pspec, gpointer user_data)
{
  (void) game;
  (void) pspec;
  refresh_details (user_data);
}

void
gog_game_detail_set_game (GogGameDetail *self, GogGame *game)
{
  if (self->game)
    {
      g_clear_signal_handler (&self->gamesdb_handler, self->game);
      g_clear_signal_handler (&self->store_handler, self->game);
    }
  g_set_object (&self->game, game);
  if (game)
    {
      self->gamesdb_handler = g_signal_connect (game, "notify::gamesdb",
                                                G_CALLBACK (on_game_gamesdb_changed), self);
      self->store_handler = g_signal_connect (game, "notify::store",
                                              G_CALLBACK (on_game_gamesdb_changed), self);
    }
  refresh (self);
}
