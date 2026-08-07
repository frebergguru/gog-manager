/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "preferences_dialog.h"
#include "proton_fetch_dialog.h"
#include "../runner.h"
#include <string.h>

typedef struct
{
  AdwPreferencesDialog *dialog;
  GtkWindow *parent;
  GogConfig *config;
  GogLibraryPathChangedFunc on_library_path_changed;
  gpointer user_data;

  AdwActionRow *library_row;
  AdwActionRow *install_root_row;
  AdwActionRow *runner_row;
  AdwActionRow *no_runners_row;
  GtkDropDown *default_runner_dropdown;
  GPtrArray *runners;

  gchar *pending_library_path;
} PrefsCtx;

static void
save_and_close (PrefsCtx *ctx)
{
  guint idx = gtk_drop_down_get_selected (ctx->default_runner_dropdown);
  if (ctx->runners && idx != GTK_INVALID_LIST_POSITION && idx < ctx->runners->len)
    {
      GogRunnerInfo *r = g_ptr_array_index (ctx->runners, idx);
      ctx->config->default_runner_kind = r->kind;
      g_free (ctx->config->default_runner_path);
      ctx->config->default_runner_path = g_strdup (r->bin_path);
    }

  GError *error = NULL;
  if (!gog_config_save (ctx->config, &error))
    {
      g_warning ("Could not save config: %s", error->message);
      g_clear_error (&error);
    }

  if (ctx->pending_library_path && ctx->on_library_path_changed)
    ctx->on_library_path_changed (ctx->pending_library_path, ctx->user_data);
}

static void
on_choose_library_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (folder)
    {
      gchar *path = g_file_get_path (folder);
      adw_action_row_set_subtitle (ctx->library_row, path);
      g_free (ctx->pending_library_path);
      ctx->pending_library_path = path;
      g_object_unref (folder);
    }
}

static void
on_choose_library_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose GOG Games Directory");
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_choose_library_response, ctx);
  g_object_unref (dialog);
}

static void
on_choose_install_root_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (folder)
    {
      gchar *path = g_file_get_path (folder);
      adw_action_row_set_subtitle (ctx->install_root_row, path);
      g_free (ctx->config->install_root);
      ctx->config->install_root = path;
      g_object_unref (folder);
    }
}

static void
on_choose_install_root_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose Default Install Directory");
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_choose_install_root_response, ctx);
  g_object_unref (dialog);
}

static void
refresh_runner_dropdown (PrefsCtx *ctx)
{
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  ctx->runners = runner_discover_all (ctx->config);

  GtkStringList *list = gtk_string_list_new (NULL);
  guint select_idx = 0;
  for (guint i = 0; i < ctx->runners->len; i++)
    {
      GogRunnerInfo *r = g_ptr_array_index (ctx->runners, i);
      gchar *label = g_strdup_printf ("%s (%s)", r->name,
                                       r->kind == GOG_RUNNER_PROTON ? "Proton" : "Wine");
      gtk_string_list_append (list, label);
      if (r->kind == ctx->config->default_runner_kind &&
          g_strcmp0 (r->bin_path, ctx->config->default_runner_path) == 0)
        select_idx = i;
      g_free (label);
    }
  gtk_drop_down_set_model (ctx->default_runner_dropdown, G_LIST_MODEL (list));
  gtk_drop_down_set_selected (ctx->default_runner_dropdown, select_idx);
  g_object_unref (list);

  gtk_widget_set_visible (GTK_WIDGET (ctx->runner_row), ctx->runners->len > 0);
  gtk_widget_set_visible (GTK_WIDGET (ctx->no_runners_row), ctx->runners->len == 0);
}

static void
on_proton_fetch_done (gboolean success, const char *result, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  if (success)
    refresh_runner_dropdown (ctx);
  (void) result;
}

static void
on_download_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  gog_show_proton_fetch_dialog (ctx->parent, on_proton_fetch_done, ctx);
}

static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  PrefsCtx *ctx = user_data;
  save_and_close (ctx);
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  g_free (ctx->pending_library_path);
  g_free (ctx);
}

void
gog_show_preferences_dialog (GtkWindow *parent, GogConfig *config,
                              GogLibraryPathChangedFunc on_library_path_changed, gpointer user_data)
{
  PrefsCtx *ctx = g_new0 (PrefsCtx, 1);
  ctx->parent = parent;
  ctx->config = config;
  ctx->on_library_path_changed = on_library_path_changed;
  ctx->user_data = user_data;

  AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG (adw_preferences_dialog_new ());
  ctx->dialog = dialog;
  adw_preferences_dialog_set_search_enabled (dialog, FALSE);

  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_set_title (page, "General");
  adw_preferences_page_set_icon_name (page, "preferences-system-symbolic");

  /* Library group */
  AdwPreferencesGroup *lib_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (lib_group, "Library");
  adw_preferences_group_set_description (lib_group, "Where your downloaded GOG installers live");

  ctx->library_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->library_row), "Library Directory");
  adw_action_row_set_subtitle (ctx->library_row,
                                config->library_path ? config->library_path : "(not set)");
  GtkWidget *lib_btn = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_valign (lib_btn, GTK_ALIGN_CENTER);
  g_signal_connect (lib_btn, "clicked", G_CALLBACK (on_choose_library_clicked), ctx);
  adw_action_row_add_suffix (ctx->library_row, lib_btn);
  adw_preferences_group_add (lib_group, GTK_WIDGET (ctx->library_row));
  adw_preferences_page_add (page, lib_group);

  /* Install group */
  AdwPreferencesGroup *install_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (install_group, "Installation");

  ctx->install_root_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->install_root_row), "Default Install Location");
  adw_action_row_set_subtitle (
      ctx->install_root_row,
      config->install_root ? config->install_root : "(choose per-install)");
  GtkWidget *root_btn = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_valign (root_btn, GTK_ALIGN_CENTER);
  g_signal_connect (root_btn, "clicked", G_CALLBACK (on_choose_install_root_clicked), ctx);
  adw_action_row_add_suffix (ctx->install_root_row, root_btn);
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->install_root_row));

  ctx->runner_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->runner_row), "Default Runner");
  adw_action_row_set_subtitle (ctx->runner_row, "Used to pre-select Wine/Proton in the install dialog");
  ctx->default_runner_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
  gtk_widget_set_valign (GTK_WIDGET (ctx->default_runner_dropdown), GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ctx->runner_row, GTK_WIDGET (ctx->default_runner_dropdown));
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->runner_row));

  ctx->no_runners_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->no_runners_row),
                                  "No Wine or Proton found on this system");
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->no_runners_row));

  AdwActionRow *download_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (download_row), "Get GE-Proton");
  adw_action_row_set_subtitle (download_row,
                                "Download the latest GE-Proton build from GitHub for playing "
                                "Windows games");
  GtkWidget *download_btn = gtk_button_new_with_label ("Download…");
  gtk_widget_set_valign (download_btn, GTK_ALIGN_CENTER);
  g_signal_connect (download_btn, "clicked", G_CALLBACK (on_download_proton_clicked), ctx);
  adw_action_row_add_suffix (download_row, download_btn);
  adw_preferences_group_add (install_group, GTK_WIDGET (download_row));

  refresh_runner_dropdown (ctx);

  adw_preferences_page_add (page, install_group);

  adw_preferences_dialog_add (dialog, page);

  g_signal_connect (dialog, "closed", G_CALLBACK (on_dialog_closed), ctx);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (parent));
}
