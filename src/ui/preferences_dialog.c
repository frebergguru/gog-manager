/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "preferences_dialog.h"
#include "dialog_window.h"
#include "proton_fetch_dialog.h"
#include "proton_manage_dialog.h"
#include "../proton_fetch.h"
#include "../runner.h"
#include "../gog_sync.h"
#include <string.h>

typedef struct
{
  GtkWindow *window;
  GtkWindow *parent;
  GogConfig *config;
  GogLibrary *library;
  GogState *state;
  GogLibraryPathChangedFunc on_library_path_changed;
  gpointer user_data;

  AdwActionRow *library_row;
  AdwActionRow *install_root_row;
  AdwActionRow *runner_row;
  AdwActionRow *no_runners_row;
  AdwActionRow *custom_runners_row;
  AdwActionRow *download_row;
  GCancellable *checking; /* the GitHub check for a newer GE-Proton */
  gchar *latest_tag;
  gchar *latest_dir;
  AdwActionRow *manage_proton_row;
  GtkWidget *manage_proton_button;
  GtkDropDown *default_runner_dropdown;
  GogRunnerInfo *picked;      /* default runner the user chose here, or NULL */
  gboolean refreshing;        /* the dropdown is being rebuilt, not picked from */
  GPtrArray *runners;

  gchar *pending_library_path;
} PrefsCtx;

static void
save_and_close (PrefsCtx *ctx)
{
  /* Only a runner picked here: the dropdown shows the first runner when
   * the saved one isn't found (e.g. an unmounted custom runner), and that
   * must not overwrite the setting. */
  if (ctx->picked)
    {
      ctx->config->default_runner_kind = ctx->picked->kind;
      g_free (ctx->config->default_runner_path);
      ctx->config->default_runner_path = g_strdup (ctx->picked->bin_path);
    }

  if (ctx->pending_library_path)
    {
      g_free (ctx->config->library_path);
      ctx->config->library_path = g_strdup (ctx->pending_library_path);
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

  /* Keep showing what was picked here, not yet saved, over the saved one. */
  GogRunnerKind want_kind = ctx->picked ? ctx->picked->kind : ctx->config->default_runner_kind;
  const char *want_path = ctx->picked ? ctx->picked->bin_path : ctx->config->default_runner_path;
  GtkStringList *list = gtk_string_list_new (NULL);
  guint select_idx = 0;
  for (guint i = 0; i < ctx->runners->len; i++)
    {
      GogRunnerInfo *r = g_ptr_array_index (ctx->runners, i);
      gchar *label = g_strdup_printf ("%s (%s)", r->name,
                                       r->kind == GOG_RUNNER_PROTON ? "Proton" : "Wine");
      gtk_string_list_append (list, label);
      if (r->kind == want_kind && g_strcmp0 (r->bin_path, want_path) == 0)
        select_idx = i;
      g_free (label);
    }
  ctx->refreshing = TRUE;
  gtk_drop_down_set_model (ctx->default_runner_dropdown, G_LIST_MODEL (list));
  gtk_drop_down_set_selected (ctx->default_runner_dropdown, select_idx);
  ctx->refreshing = FALSE;
  g_object_unref (list);

  gtk_widget_set_visible (GTK_WIDGET (ctx->runner_row), ctx->runners->len > 0);
  gtk_widget_set_visible (GTK_WIDGET (ctx->no_runners_row), ctx->runners->len == 0);
}

static void
on_default_runner_selected (GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  PrefsCtx *ctx = user_data;
  if (ctx->refreshing)
    return;
  guint idx = gtk_drop_down_get_selected (dropdown);
  if (!ctx->runners || idx == GTK_INVALID_LIST_POSITION || idx >= ctx->runners->len)
    return;
  GogRunnerInfo *r = g_ptr_array_index (ctx->runners, idx);
  gog_runner_info_free (ctx->picked);
  ctx->picked = gog_runner_info_new (r->kind, r->name, r->bin_path);
}

static void
refresh_custom_runners_row (PrefsCtx *ctx)
{
  guint n = ctx->config->custom_runner_paths ? g_strv_length (ctx->config->custom_runner_paths) : 0;
  gchar *subtitle = n == 0 ? g_strdup ("A Wine binary or Proton folder that isn't found automatically")
                           : g_strdup_printf ("%u added", n);
  adw_action_row_set_subtitle (ctx->custom_runners_row, subtitle);
  g_free (subtitle);
}

/* Stored as "wine=<path>" / "proton=<path>", the encoding
 * runner_discover_all() reads back. */
static void
add_custom_runner (PrefsCtx *ctx, const char *kind_prefix, GFile *file)
{
  gchar *path = g_file_get_path (file);
  if (!path)
    return;
  gchar *entry = g_strconcat (kind_prefix, path, NULL);
  g_free (path);

  GStrv old = ctx->config->custom_runner_paths;
  if (old && g_strv_contains ((const char *const *) old, entry))
    {
      g_free (entry);
      return;
    }
  GStrvBuilder *builder = g_strv_builder_new ();
  if (old)
    g_strv_builder_addv (builder, (const char **) old);
  g_strv_builder_add (builder, entry);
  g_free (entry);
  ctx->config->custom_runner_paths = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  g_strfreev (old);

  refresh_runner_dropdown (ctx);
  refresh_custom_runners_row (ctx);
}

static void
on_add_wine_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (file)
    {
      add_custom_runner (user_data, "wine=", file);
      g_object_unref (file);
    }
}

static void
on_add_wine_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose a Wine Binary");
  gtk_file_dialog_open (dialog, ctx->parent, NULL, on_add_wine_response, ctx);
  g_object_unref (dialog);
}

static void
on_add_proton_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (folder)
    {
      add_custom_runner (user_data, "proton=", folder);
      g_object_unref (folder);
    }
}

static void
on_add_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose a Proton Folder");
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_add_proton_response, ctx);
  g_object_unref (dialog);
}

static void
refresh_manage_proton_row (PrefsCtx *ctx)
{
  GPtrArray *names = proton_fetch_list_installed ();
  gchar *subtitle = names->len == 0 ? g_strdup ("None downloaded yet")
                    : names->len == 1
                        ? g_strdup_printf ("1 version: %s", (const char *) names->pdata[0])
                        : g_strdup_printf ("%u versions, newest %s", names->len,
                                           (const char *) names->pdata[0]);
  adw_action_row_set_subtitle (ctx->manage_proton_row, subtitle);
  gtk_widget_set_sensitive (ctx->manage_proton_button, names->len > 0);
  g_free (subtitle);
  g_ptr_array_unref (names);
}

static void
refresh_update_status (PrefsCtx *ctx)
{
  if (!ctx->latest_tag)
    return;
  gchar *subtitle = proton_fetch_is_installed (ctx->latest_tag, ctx->latest_dir)
                        ? g_strdup_printf ("You have the newest version, %s", ctx->latest_tag)
                        : g_strdup_printf ("Update available: %s. Download it to use it for "
                                           "new installs.",
                                           ctx->latest_tag);
  adw_action_row_set_subtitle (ctx->download_row, subtitle);
  g_free (subtitle);
}

static void
on_latest_checked (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  gchar *tag = NULL, *dir_name = NULL;
  GError *error = NULL;
  if (!proton_fetch_check_latest_finish (res, &tag, &dir_name, &error))
    {
      /* Cancelled means the dialog is gone. Offline or rate-limited: keep
       * the plain description rather than nagging. */
      gboolean cancelled = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
      g_clear_error (&error);
      if (!cancelled)
        g_clear_object (&((PrefsCtx *) user_data)->checking);
      return;
    }
  PrefsCtx *ctx = user_data;
  g_clear_object (&ctx->checking);
  g_free (ctx->latest_tag);
  g_free (ctx->latest_dir);
  ctx->latest_tag = tag;
  ctx->latest_dir = dir_name;
  refresh_update_status (ctx);
}

static void
on_proton_fetch_done (gboolean success, const char *result, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  if (success)
    {
      refresh_runner_dropdown (ctx);
      refresh_manage_proton_row (ctx);
      refresh_update_status (ctx);
    }
  (void) result;
}

static void
on_proton_manage_done (gboolean changed, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  if (changed)
    {
      refresh_runner_dropdown (ctx);
      refresh_manage_proton_row (ctx);
      refresh_update_status (ctx);
    }
}

static void
on_manage_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  gog_show_proton_manage_dialog (ctx->parent, ctx->config, ctx->library, ctx->state,
                                 on_proton_manage_done, ctx);
}

static void
on_download_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  gog_show_proton_fetch_dialog (ctx->parent, ctx->config, ctx->library, on_proton_fetch_done,
                                ctx);
}

/* ---- default download languages ---- */

static void
refresh_other_languages (AdwExpanderRow *row)
{
  guint mask = gog_sync_get_default_languages ();
  gchar *text;
  if (mask)
    text = gog_sync_describe_languages (mask);
  else if (gog_sync_get_default_original ())
    text = g_strdup ("None");
  else
    text = g_strdup ("None, so every language is downloaded");
  adw_expander_row_set_subtitle (row, text);
  g_free (text);
}

static void
on_default_original_changed (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  gog_sync_set_default_original (adw_switch_row_get_active (row));
  refresh_other_languages (user_data);
}

static void
on_default_language_toggled (GtkCheckButton *check, gpointer user_data)
{
  guint bit = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (check), "bit"));
  guint mask = gog_sync_get_default_languages ();
  mask = gtk_check_button_get_active (check) ? (mask | bit) : (mask & ~bit);
  gog_sync_set_default_languages (mask);
  refresh_other_languages (user_data);
}

/* Which languages GOG downloads come in, for games with no choice of their
 * own in New Games and Updates on GOG. */
static AdwPreferencesGroup *
download_languages_group (void)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (group, "Download Languages");
  adw_preferences_group_set_description (
      group, "The languages new games and updates from GOG come in. A game's own choice in "
             "New Games and Updates on GOG goes before these.");

  AdwExpanderRow *others = ADW_EXPANDER_ROW (adw_expander_row_new ());

  GtkWidget *original = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (original), "Original Language");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (original),
                               "Each game's original language, from Wikidata: a game made in "
                               "Norway counts as Norwegian. English when it isn't known.");
  adw_switch_row_set_active (ADW_SWITCH_ROW (original), gog_sync_get_default_original ());
  g_signal_connect (original, "notify::active", G_CALLBACK (on_default_original_changed), others);
  adw_preferences_group_add (group, original);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (others), "Other Languages");
  guint n_langs;
  const GogSyncLanguage *langs = gog_sync_get_languages (&n_langs);
  guint chosen = gog_sync_get_default_languages ();
  for (guint i = 0; i < n_langs; i++)
    {
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), langs[i].name);
      GtkWidget *check = gtk_check_button_new ();
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (check), (chosen & (1u << i)) != 0);
      gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      langs[i].name, -1);
      g_object_set_data (G_OBJECT (check), "bit", GUINT_TO_POINTER (1u << i));
      g_signal_connect (check, "toggled", G_CALLBACK (on_default_language_toggled), others);
      adw_action_row_add_prefix (ADW_ACTION_ROW (row), check);
      adw_action_row_set_activatable_widget (ADW_ACTION_ROW (row), check);
      adw_expander_row_add_row (others, row);
    }
  refresh_other_languages (others);
  adw_preferences_group_add (group, GTK_WIDGET (others));
  return group;
}

/* Saves while the window's widgets still exist; destroy comes too late. */
static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  (void) window;
  save_and_close (user_data);
  return FALSE;
}

static void
on_window_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  PrefsCtx *ctx = user_data;
  if (ctx->checking)
    {
      g_cancellable_cancel (ctx->checking);
      g_object_unref (ctx->checking);
    }
  g_free (ctx->latest_tag);
  g_free (ctx->latest_dir);
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  gog_runner_info_free (ctx->picked);
  g_free (ctx->pending_library_path);
  g_free (ctx);
}

void
gog_show_preferences_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                              GogState *state, GogLibraryPathChangedFunc on_library_path_changed,
                              gpointer user_data)
{
  PrefsCtx *ctx = g_new0 (PrefsCtx, 1);
  ctx->parent = parent;
  ctx->config = config;
  ctx->library = library;
  ctx->state = state;
  ctx->on_library_path_changed = on_library_path_changed;
  ctx->user_data = user_data;

  ctx->window = gog_dialog_window_new (parent, "Preferences", 640, 600, 360, 300);

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
                                config->library_path ? config->library_path
                                                     : "(not set: the current folder is scanned)");
  GtkWidget *lib_btn = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_valign (lib_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (lib_btn, "Choose library directory");
  gtk_accessible_update_property (GTK_ACCESSIBLE (lib_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Choose library directory", -1);
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
      config->install_root ? config->install_root : "(not set: ~/GOG Games)");
  GtkWidget *root_btn = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_valign (root_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (root_btn, "Choose default install location");
  gtk_accessible_update_property (GTK_ACCESSIBLE (root_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Choose default install location", -1);
  g_signal_connect (root_btn, "clicked", G_CALLBACK (on_choose_install_root_clicked), ctx);
  adw_action_row_add_suffix (ctx->install_root_row, root_btn);
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->install_root_row));

  ctx->runner_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->runner_row), "Default Runner");
  adw_action_row_set_subtitle (ctx->runner_row, "Used to pre-select Wine/Proton in the install dialog");
  ctx->default_runner_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
  gtk_widget_set_valign (GTK_WIDGET (ctx->default_runner_dropdown), GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->default_runner_dropdown),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Default Runner", -1);
  adw_action_row_add_suffix (ctx->runner_row, GTK_WIDGET (ctx->default_runner_dropdown));
  g_signal_connect (ctx->default_runner_dropdown, "notify::selected",
                    G_CALLBACK (on_default_runner_selected), ctx);
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->runner_row));

  ctx->no_runners_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->no_runners_row),
                                  "No Wine or Proton found on this system");
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->no_runners_row));

  ctx->custom_runners_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->custom_runners_row), "Custom Runners");
  GtkWidget *add_wine_btn = gtk_button_new_with_label ("Add Wine…");
  gtk_widget_set_valign (add_wine_btn, GTK_ALIGN_CENTER);
  g_signal_connect (add_wine_btn, "clicked", G_CALLBACK (on_add_wine_clicked), ctx);
  adw_action_row_add_suffix (ctx->custom_runners_row, add_wine_btn);
  GtkWidget *add_proton_btn = gtk_button_new_with_label ("Add Proton…");
  gtk_widget_set_valign (add_proton_btn, GTK_ALIGN_CENTER);
  g_signal_connect (add_proton_btn, "clicked", G_CALLBACK (on_add_proton_clicked), ctx);
  adw_action_row_add_suffix (ctx->custom_runners_row, add_proton_btn);
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->custom_runners_row));
  refresh_custom_runners_row (ctx);

  AdwActionRow *download_row = ADW_ACTION_ROW (adw_action_row_new ());
  ctx->download_row = download_row;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (download_row), "Get GE-Proton");
  adw_action_row_set_subtitle (download_row,
                                "Download the latest GE-Proton build from GitHub for playing "
                                "Windows games");
  GtkWidget *download_btn = gtk_button_new_with_label ("Download…");
  gtk_widget_set_valign (download_btn, GTK_ALIGN_CENTER);
  g_signal_connect (download_btn, "clicked", G_CALLBACK (on_download_proton_clicked), ctx);
  adw_action_row_add_suffix (download_row, download_btn);
  adw_preferences_group_add (install_group, GTK_WIDGET (download_row));

  ctx->manage_proton_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->manage_proton_row),
                                 "Downloaded GE-Proton");
  ctx->manage_proton_button = gtk_button_new_with_label ("Manage…");
  gtk_widget_set_valign (ctx->manage_proton_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (ctx->manage_proton_button, "Remove old GE-Proton versions");
  g_signal_connect (ctx->manage_proton_button, "clicked", G_CALLBACK (on_manage_proton_clicked),
                    ctx);
  adw_action_row_add_suffix (ctx->manage_proton_row, ctx->manage_proton_button);
  adw_preferences_group_add (install_group, GTK_WIDGET (ctx->manage_proton_row));
  refresh_manage_proton_row (ctx);

  ctx->checking = g_cancellable_new ();
  proton_fetch_check_latest_async (ctx->checking, on_latest_checked, ctx);

  refresh_runner_dropdown (ctx);

  adw_preferences_page_add (page, install_group);
  adw_preferences_page_add (page, download_languages_group ());

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (page));
  adw_window_set_content (ADW_WINDOW (ctx->window), toolbar_view);

  g_signal_connect (ctx->window, "close-request", G_CALLBACK (on_close_request), ctx);
  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (ctx->window);
}
