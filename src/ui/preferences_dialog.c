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
  GogRefetchCategoriesFunc on_refetch_categories;
  gpointer user_data;

  AdwActionRow *library_row;
  AdwActionRow *install_root_row;
  AdwComboRow *runner_row;
  AdwActionRow *no_runners_row;
  AdwPreferencesGroup *custom_group;
  GPtrArray *custom_rows; /* the rows listing custom runners, to rebuild */
  AdwActionRow *download_row;
  AdwActionRow *categories_row;
  GCancellable *checking; /* the GitHub check for a newer GE-Proton */
  gchar *latest_tag;
  gchar *latest_dir;
  AdwActionRow *manage_proton_row;
  GtkWidget *manage_proton_button;
  GogRunnerInfo *picked;      /* default runner the user chose here, or NULL */
  gboolean refreshing;        /* the dropdown is being rebuilt, not picked from */
  GPtrArray *runners;

  gchar *pending_library_path;
} PrefsCtx;

/* Callbacks that can outlive the window (file choosers, the Proton
 * windows) get a ref on it and find ctx through it; NULL once it is gone. */
#define CTX_KEY "gog-prefs-ctx"

static gpointer
ref_window (PrefsCtx *ctx)
{
  return g_object_ref (ctx->window);
}

/* Takes the ref from ref_window(). */
static PrefsCtx *
live_ctx (gpointer window)
{
  PrefsCtx *ctx = g_object_get_data (G_OBJECT (window), CTX_KEY);
  g_object_unref (window);
  return ctx;
}

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
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  PrefsCtx *ctx = live_ctx (user_data);
  if (folder && !ctx)
    g_clear_object (&folder);
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
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_choose_library_response, ref_window (ctx));
  g_object_unref (dialog);
}

static void
on_choose_install_root_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  PrefsCtx *ctx = live_ctx (user_data);
  if (folder && !ctx)
    g_clear_object (&folder);
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
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_choose_install_root_response, ref_window (ctx));
  g_object_unref (dialog);
}

static void
refresh_runner_dropdown (PrefsCtx *ctx)
{
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  ctx->runners = runner_discover_all (ctx->config);

  /* First, when there is a Proton: "Newest Proton", which follows new
   * GE-Proton downloads. Its entry stands for no runner of its own. */
  GogRunnerInfo *newest = runner_find_newest_proton (ctx->runners);
  if (newest)
    {
      gchar *label = g_strdup_printf ("Newest Proton (now %s)", newest->name);
      g_ptr_array_insert (ctx->runners, 0,
                          gog_runner_info_new (GOG_RUNNER_PROTON, label, GOG_RUNNER_NEWEST_PROTON));
      g_free (label);
    }

  /* Keep showing what was picked here, not yet saved, over the saved one. */
  GogRunnerKind want_kind = ctx->picked ? ctx->picked->kind : ctx->config->default_runner_kind;
  const char *want_path = ctx->picked ? ctx->picked->bin_path : ctx->config->default_runner_path;
  /* No default yet means Newest Proton. */
  if (newest && (want_kind == GOG_RUNNER_NONE || !want_path))
    {
      want_kind = GOG_RUNNER_PROTON;
      want_path = GOG_RUNNER_NEWEST_PROTON;
    }
  GtkStringList *list = gtk_string_list_new (NULL);
  guint select_idx = 0;
  gboolean found = FALSE;
  for (guint i = 0; i < ctx->runners->len; i++)
    {
      GogRunnerInfo *r = g_ptr_array_index (ctx->runners, i);
      gchar *label = newest && i == 0
                         ? g_strdup (r->name)
                         : g_strdup_printf ("%s (%s)", r->name,
                                            r->kind == GOG_RUNNER_PROTON ? "Proton" : "Wine");
      gtk_string_list_append (list, label);
      if (r->kind == want_kind && g_strcmp0 (r->bin_path, want_path) == 0)
        {
          select_idx = i;
          found = TRUE;
        }
      g_free (label);
    }
  /* A pick that is gone (removed in Manage) must not be saved back. */
  if (!found && ctx->picked)
    g_clear_pointer (&ctx->picked, gog_runner_info_free);
  ctx->refreshing = TRUE;
  adw_combo_row_set_model (ctx->runner_row, G_LIST_MODEL (list));
  adw_combo_row_set_selected (ctx->runner_row, select_idx);
  ctx->refreshing = FALSE;
  g_object_unref (list);

  gtk_widget_set_visible (GTK_WIDGET (ctx->runner_row), ctx->runners->len > 0);
  gtk_widget_set_visible (GTK_WIDGET (ctx->no_runners_row), ctx->runners->len == 0);
}

static void
on_default_runner_selected (AdwComboRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  PrefsCtx *ctx = user_data;
  if (ctx->refreshing)
    return;
  guint idx = adw_combo_row_get_selected (row);
  if (!ctx->runners || idx == GTK_INVALID_LIST_POSITION || idx >= ctx->runners->len)
    return;
  GogRunnerInfo *r = g_ptr_array_index (ctx->runners, idx);
  gog_runner_info_free (ctx->picked);
  ctx->picked = gog_runner_info_new (r->kind, r->name, r->bin_path);
}

static void refresh_custom_runners (PrefsCtx *ctx);

static void
on_remove_custom_runner_clicked (GtkButton *button, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  const char *entry = g_object_get_data (G_OBJECT (button), "entry");
  GStrvBuilder *builder = g_strv_builder_new ();
  for (GStrv p = ctx->config->custom_runner_paths; p && *p; p++)
    if (g_strcmp0 (*p, entry) != 0)
      g_strv_builder_add (builder, *p);
  /* entry belongs to the button, which the rebuild below destroys. */
  GStrv old = ctx->config->custom_runner_paths;
  ctx->config->custom_runner_paths = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  refresh_runner_dropdown (ctx);
  refresh_custom_runners (ctx);
  g_strfreev (old);
}

/* One row per custom runner, with a Remove button; a placeholder when
 * there are none. */
static void
refresh_custom_runners (PrefsCtx *ctx)
{
  for (guint i = 0; i < ctx->custom_rows->len; i++)
    adw_preferences_group_remove (ctx->custom_group, g_ptr_array_index (ctx->custom_rows, i));
  g_ptr_array_set_size (ctx->custom_rows, 0);

  for (GStrv p = ctx->config->custom_runner_paths; p && *p; p++)
    {
      gboolean proton = g_str_has_prefix (*p, "proton=");
      const char *path = strchr (*p, '=') ? strchr (*p, '=') + 1 : *p;
      gchar *name = g_path_get_basename (path);
      if (!proton)
        {
          /* ".../wine-9.0/bin/wine": the folder says which Wine it is. */
          gchar *bin = g_path_get_dirname (path);
          gchar *top = g_path_get_dirname (bin);
          if (g_str_has_suffix (bin, "/bin") && strcmp (top, "/usr") != 0 && strcmp (top, "/") != 0)
            {
              g_free (name);
              name = g_path_get_basename (top);
            }
          g_free (bin);
          g_free (top);
        }
      GtkWidget *row = adw_action_row_new ();
      gchar *title = g_strdup_printf ("%s (%s)", name, proton ? "Proton" : "Wine");
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), path);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
      GtkWidget *remove = gtk_button_new_from_icon_name ("user-trash-symbolic");
      gtk_widget_add_css_class (remove, "flat");
      gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
      gchar *label = g_strdup_printf ("Remove %s", title);
      gtk_widget_set_tooltip_text (remove, label);
      gtk_accessible_update_property (GTK_ACCESSIBLE (remove), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label, -1);
      g_object_set_data_full (G_OBJECT (remove), "entry", g_strdup (*p), g_free);
      g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_custom_runner_clicked), ctx);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), remove);
      adw_preferences_group_add (ctx->custom_group, row);
      g_ptr_array_add (ctx->custom_rows, row);
      g_free (label);
      g_free (title);
      g_free (name);
    }

  if (ctx->custom_rows->len == 0)
    {
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), "None added");
      adw_preferences_group_add (ctx->custom_group, row);
      g_ptr_array_add (ctx->custom_rows, row);
    }
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
  refresh_custom_runners (ctx);
}

static void
on_add_wine_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
  PrefsCtx *ctx = live_ctx (user_data);
  if (file && !ctx)
    g_clear_object (&file);
  if (file)
    {
      add_custom_runner (ctx, "wine=", file);
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
  gtk_file_dialog_open (dialog, ctx->parent, NULL, on_add_wine_response, ref_window (ctx));
  g_object_unref (dialog);
}

static void
on_add_proton_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  PrefsCtx *ctx = live_ctx (user_data);
  if (folder && !ctx)
    g_clear_object (&folder);
  if (folder)
    {
      add_custom_runner (ctx, "proton=", folder);
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
  gtk_file_dialog_select_folder (dialog, ctx->parent, NULL, on_add_proton_response, ref_window (ctx));
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
        {
          PrefsCtx *ctx = user_data;
          g_clear_object (&ctx->checking);
          adw_action_row_set_subtitle (ctx->download_row,
                                       "Download the newest GE-Proton build from GitHub");
        }
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
  PrefsCtx *ctx = live_ctx (user_data);
  if (success && ctx)
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
  PrefsCtx *ctx = live_ctx (user_data);
  if (changed && ctx)
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
                                 on_proton_manage_done, ref_window (ctx));
}

static void
on_download_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsCtx *ctx = user_data;
  gog_show_proton_fetch_dialog (ctx->parent, ctx->config, ctx->library, on_proton_fetch_done,
                                ref_window (ctx));
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
      group, "The languages new games and updates from GOG come in, unless a game has its "
             "own choice in New Games and Updates on GOG.");

  AdwExpanderRow *others = ADW_EXPANDER_ROW (adw_expander_row_new ());

  GtkWidget *original = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (original), "Original Language");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (original),
                               "From Wikidata; a game made in Norway counts as Norwegian. "
                               "English when it isn't known.");
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
  PrefsCtx *ctx = user_data;
  g_object_set_data (G_OBJECT (window), CTX_KEY, NULL);
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
  g_ptr_array_unref (ctx->custom_rows);
  g_free (ctx->pending_library_path);
  g_free (ctx);
}

static void
on_refetch_clicked (GtkButton *button, gpointer user_data)
{
  PrefsCtx *ctx = user_data;
  if (ctx->on_refetch_categories)
    ctx->on_refetch_categories (ctx->user_data);
  /* Once per visit is enough; the main window shows the progress. */
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  adw_action_row_set_subtitle (ctx->categories_row,
                               "Fetching in the background; the bar at the bottom of the "
                               "library shows the progress.");
}

static AdwPreferencesGroup *
categories_group (PrefsCtx *ctx)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (group, "Details from GOG");
  adw_preferences_group_set_description (
      group, "Categories, box art and details are fetched once per game and kept for offline "
             "use.");
  ctx->categories_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->categories_row), "Fetch Everything Again");
  adw_action_row_set_subtitle (ctx->categories_row, "If GOG has changed a game's genres or art");
  GtkWidget *button = gtk_button_new_with_label ("Fetch Again");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Fetch categories, covers and details again", -1);
  g_signal_connect (button, "clicked", G_CALLBACK (on_refetch_clicked), ctx);
  adw_action_row_add_suffix (ctx->categories_row, button);
  adw_preferences_group_add (group, GTK_WIDGET (ctx->categories_row));
  return group;
}

/* A row for a folder: the path is the main text, and the whole row opens
 * the folder chooser. */
static AdwActionRow *
folder_row (const char *title, const char *path, const char *button_label, GCallback clicked,
            PrefsCtx *ctx)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  gtk_widget_add_css_class (GTK_WIDGET (row), "property");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_action_row_set_subtitle (row, path);
  GtkWidget *button = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (button, button_label);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  button_label, -1);
  g_signal_connect (button, "clicked", clicked, ctx);
  adw_action_row_add_suffix (row, button);
  adw_action_row_set_activatable_widget (row, button);
  return row;
}

static GtkWidget *
row_button (const char *label, GCallback clicked, PrefsCtx *ctx)
{
  GtkWidget *button = gtk_button_new_with_label (label);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  g_signal_connect (button, "clicked", clicked, ctx);
  return button;
}

static AdwPreferencesPage *
library_page (PrefsCtx *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *folders = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (folders, "Folders");
  ctx->library_row = folder_row ("Game Library",
                                 ctx->config->library_path
                                     ? ctx->config->library_path
                                     : "Not set: the folder GOG Manager starts in is used",
                                 "Choose the game library folder",
                                 G_CALLBACK (on_choose_library_clicked), ctx);
  adw_preferences_group_add (folders, GTK_WIDGET (ctx->library_row));
  ctx->install_root_row = folder_row ("New Games Are Installed In",
                                      ctx->config->install_root ? ctx->config->install_root
                                                                : "~/GOG Games",
                                      "Choose where new games are installed",
                                      G_CALLBACK (on_choose_install_root_clicked), ctx);
  adw_preferences_group_add (folders, GTK_WIDGET (ctx->install_root_row));
  adw_preferences_page_add (page, folders);

  adw_preferences_page_add (page, categories_group (ctx));
  return page;
}

static AdwPreferencesPage *
runners_page (PrefsCtx *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());

  AdwPreferencesGroup *defaults = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (defaults, "Default Runner");
  adw_preferences_group_set_description (defaults,
                                         "Pre-selected when a Windows game is installed. "
                                         "Installed games keep their own runner.");
  ctx->runner_row = ADW_COMBO_ROW (adw_combo_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->runner_row), "Runner");
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (ctx->runner_row), FALSE);
  /* The choice on its own line: Proton names are long. */
  adw_combo_row_set_use_subtitle (ctx->runner_row, TRUE);
  g_signal_connect (ctx->runner_row, "notify::selected", G_CALLBACK (on_default_runner_selected),
                    ctx);
  adw_preferences_group_add (defaults, GTK_WIDGET (ctx->runner_row));
  ctx->no_runners_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->no_runners_row),
                                 "No Wine or Proton found");
  adw_action_row_set_subtitle (ctx->no_runners_row, "Download GE-Proton below, or add a runner");
  adw_preferences_group_add (defaults, GTK_WIDGET (ctx->no_runners_row));
  adw_preferences_page_add (page, defaults);

  AdwPreferencesGroup *proton = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (proton, "GE-Proton");
  adw_preferences_group_set_description (proton, "Runs Windows games. Downloaded from GitHub.");
  ctx->download_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->download_row), "Newest Version");
  adw_action_row_set_subtitle (ctx->download_row, "Checking GitHub…");
  adw_action_row_add_suffix (ctx->download_row,
                             row_button ("Download…", G_CALLBACK (on_download_proton_clicked), ctx));
  adw_preferences_group_add (proton, GTK_WIDGET (ctx->download_row));
  ctx->manage_proton_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->manage_proton_row),
                                 "Downloaded Versions");
  ctx->manage_proton_button = row_button ("Manage…", G_CALLBACK (on_manage_proton_clicked), ctx);
  gtk_widget_set_tooltip_text (ctx->manage_proton_button, "Remove old GE-Proton versions");
  adw_action_row_add_suffix (ctx->manage_proton_row, ctx->manage_proton_button);
  adw_preferences_group_add (proton, GTK_WIDGET (ctx->manage_proton_row));
  adw_preferences_page_add (page, proton);

  ctx->custom_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_group_set_title (ctx->custom_group, "Custom Runners");
  adw_preferences_group_set_description (ctx->custom_group,
                                         "Wine or Proton that isn't found automatically");
  GtkWidget *add_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append (GTK_BOX (add_box), row_button ("Add Wine…", G_CALLBACK (on_add_wine_clicked), ctx));
  gtk_box_append (GTK_BOX (add_box),
                  row_button ("Add Proton…", G_CALLBACK (on_add_proton_clicked), ctx));
  adw_preferences_group_set_header_suffix (ctx->custom_group, add_box);
  adw_preferences_page_add (page, ctx->custom_group);
  return page;
}

static void
add_page (AdwViewStack *stack, AdwPreferencesPage *page, const char *name, const char *title,
          const char *icon)
{
  adw_view_stack_add_titled_with_icon (stack, GTK_WIDGET (page), name, title, icon);
}

void
gog_show_preferences_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                              GogState *state, GogLibraryPathChangedFunc on_library_path_changed,
                              GogRefetchCategoriesFunc on_refetch_categories, gpointer user_data)
{
  PrefsCtx *ctx = g_new0 (PrefsCtx, 1);
  ctx->parent = parent;
  ctx->config = config;
  ctx->library = library;
  ctx->state = state;
  ctx->on_library_path_changed = on_library_path_changed;
  ctx->on_refetch_categories = on_refetch_categories;
  ctx->user_data = user_data;
  ctx->custom_rows = g_ptr_array_new ();

  ctx->window = gog_dialog_window_new (parent, "Preferences", 720, 640, 360, 300);

  /* Three short pages instead of one long one. */
  AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
  add_page (stack, library_page (ctx), "library", "Library", "folder-symbolic");
  add_page (stack, runners_page (ctx), "runners", "Runners", "applications-games-symbolic");
  AdwPreferencesPage *downloads = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_add (downloads, download_languages_group ());
  add_page (stack, downloads, "downloads", "Downloads", "folder-download-symbolic");

  refresh_custom_runners (ctx);
  refresh_manage_proton_row (ctx);
  refresh_runner_dropdown (ctx);
  ctx->checking = g_cancellable_new ();
  proton_fetch_check_latest_async (ctx->checking, on_latest_checked, ctx);

  /* The switcher sits in the header when the window is wide enough, and
   * moves to a bar at the bottom when it isn't. */
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *switcher = adw_view_switcher_new ();
  adw_view_switcher_set_stack (ADW_VIEW_SWITCHER (switcher), stack);
  adw_view_switcher_set_policy (ADW_VIEW_SWITCHER (switcher), ADW_VIEW_SWITCHER_POLICY_WIDE);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), switcher);
  GtkWidget *switcher_bar = adw_view_switcher_bar_new ();
  adw_view_switcher_bar_set_stack (ADW_VIEW_SWITCHER_BAR (switcher_bar), stack);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), switcher_bar);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (stack));
  adw_window_set_content (ADW_WINDOW (ctx->window), toolbar_view);

  AdwBreakpoint *narrow = adw_breakpoint_new (adw_breakpoint_condition_parse ("max-width: 500sp"));
  GValue no_title = G_VALUE_INIT;
  g_value_init (&no_title, G_TYPE_OBJECT);
  adw_breakpoint_add_setter (narrow, G_OBJECT (header), "title-widget", &no_title);
  g_value_unset (&no_title);
  GValue reveal = G_VALUE_INIT;
  g_value_init (&reveal, G_TYPE_BOOLEAN);
  g_value_set_boolean (&reveal, TRUE);
  adw_breakpoint_add_setter (narrow, G_OBJECT (switcher_bar), "reveal", &reveal);
  g_value_unset (&reveal);
  adw_window_add_breakpoint (ADW_WINDOW (ctx->window), narrow);

  g_signal_connect (ctx->window, "close-request", G_CALLBACK (on_close_request), ctx);
  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_window_destroy), ctx);
  g_object_set_data (G_OBJECT (ctx->window), CTX_KEY, ctx);
  gtk_window_present (ctx->window);
}
