/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "install_dialog.h"
#include "proton_fetch_dialog.h"
#include "../runner.h"
#include "../installer_engine.h"
#include "dialog_window.h"

typedef struct
{
  GtkWindow *window;
  GtkWindow *parent;
  GogGame *game;
  GogInstallerInfo *installer;
  GogConfig *config;
  GogLibrary *library;
  GogState *state;
  GogInstallDoneFunc on_done;
  gpointer user_data;

  GPtrArray *runners; /* GogRunnerInfo*, only for windows installers */
  GogRunnerInfo *chosen_runner; /* copy of the runner the job was started with */
  gboolean runner_locked;       /* DLC: must reuse the base game's runner/prefix */
  GtkDropDown *runner_dropdown;
  GtkLabel *install_dir_label;
  gchar *install_dir;

  GtkWidget *setup_box;   /* runner/dir pickers + Install button */
  GtkWidget *progress_box; /* progress bar + log, shown once running */
  GtkProgressBar *progress_bar;
  GtkTextView *log_view;
  GtkButton *action_button; /* Install -> becomes Close */
  GtkButton *cancel_button;

  GogInstallJob *job;
  gboolean finished;          /* the job has reported back */
  gboolean succeeded;
  GCancellable *choosing;     /* the folder picker, if open */
} InstallCtx;

static void
ctx_free (InstallCtx *ctx)
{
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  gog_runner_info_free (ctx->chosen_runner);
  g_free (ctx->install_dir);
  if (ctx->choosing)
    {
      g_cancellable_cancel (ctx->choosing);
      g_clear_object (&ctx->choosing);
    }
  if (ctx->window)
    g_object_set_data (G_OBJECT (ctx->window), "install-ctx", NULL);
  if (ctx->job)
    {
      /* The window can go without Cancel (e.g. the main window closed
       * under it): stop the install rather than leave Wine running, or a
       * paused install waiting forever. */
      if (!ctx->finished)
        gog_install_job_cancel (ctx->job);
      /* The job's queued idles may still fire after the dialog is gone. */
      g_signal_handlers_disconnect_by_data (ctx->job, ctx);
      g_clear_object (&ctx->job);
    }
  g_clear_object (&ctx->game);
  g_free (ctx);
}

static void
append_log_line (InstallCtx *ctx, const char *line)
{
  GtkTextBuffer *buf = gtk_text_view_get_buffer (ctx->log_view);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buf, &end);
  gtk_text_buffer_insert (buf, &end, line, -1);
  gtk_text_buffer_insert (buf, &end, "\n", -1);

  GtkTextMark *mark = gtk_text_buffer_get_insert (buf);
  gtk_text_buffer_get_end_iter (buf, &end);
  gtk_text_buffer_move_mark (buf, mark, &end);
  gtk_text_view_scroll_mark_onscreen (ctx->log_view, mark);
}

static void
on_job_progress (GogInstallJob *job, gdouble fraction, const gchar *message, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  (void) job;
  if (fraction < 0)
    gtk_progress_bar_pulse (ctx->progress_bar);
  else
    gtk_progress_bar_set_fraction (ctx->progress_bar, fraction);
  if (message)
    gtk_progress_bar_set_text (ctx->progress_bar, message);
}

static void
on_copy_command_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  const char *command = g_object_get_data (G_OBJECT (button), "gog-command");
  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (button)), command);
  gtk_widget_set_tooltip_text (GTK_WIDGET (button), "Copied");
}

/* One row per distribution: name, the command (selectable), a copy button. */
static GtkWidget *
fonts_fix_list (void)
{
  GPtrArray *fixes = runner_freetype_fixes (NULL);
  GtkWidget *list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (list, "boxed-list");
  for (guint i = 0; i < fixes->len; i++)
    {
      GogFixCommand *fix = g_ptr_array_index (fixes, i);
      GtkWidget *row = adw_action_row_new ();
      /* Commands contain "&&", which markup would reject. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), fix->systems);
      gchar *subtitle = fix->note ? g_strdup_printf ("%s\n%s", fix->command, fix->note)
                                  : g_strdup (fix->command);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
      g_free (subtitle);

      GtkWidget *copy = gtk_button_new_from_icon_name ("edit-copy-symbolic");
      gtk_widget_set_valign (copy, GTK_ALIGN_CENTER);
      gtk_widget_add_css_class (copy, "flat");
      gtk_widget_set_tooltip_text (copy, "Copy command");
      gchar *label = g_strdup_printf ("Copy the command for %s", fix->systems);
      gtk_accessible_update_property (GTK_ACCESSIBLE (copy), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
      g_free (label);
      g_object_set_data_full (G_OBJECT (copy), "gog-command", g_strdup (fix->command), g_free);
      g_signal_connect (copy, "clicked", G_CALLBACK (on_copy_command_clicked), NULL);
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), copy);
      gtk_list_box_append (GTK_LIST_BOX (list), row);
    }
  g_ptr_array_unref (fixes);

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), list);
  return scroll;
}

static void
on_fonts_response (const char *response, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  /* The install can finish while the question is still open. */
  if (!ctx->job || ctx->finished)
    return;
  if (g_strcmp0 (response, "cancel") == 0)
    gog_install_job_cancel (ctx->job);
  else
    gog_install_job_resume (ctx->job);
}

static void
on_job_fonts_missing (GogInstallJob *job, gboolean paused, gpointer user_data)
{
  (void) job;
  InstallCtx *ctx = user_data;
  gchar *summary = NULL;
  g_ptr_array_unref (runner_freetype_fixes (&summary));
  gchar *body = g_strdup_printf (
      paused ? "%s\n\nThe install is paused. Run the command for your system in a terminal, "
               "then choose Continue."
             : "%s\n\nThe installer is already running. If it stops making progress, cancel, "
               "run the command for your system, and start the install again.",
      summary);
  GogAlert *alert = gog_alert_new ("Fonts Missing for Windows Programs", body);
  g_free (body);
  g_free (summary);
  gog_alert_set_extra_child (alert, fonts_fix_list ());
  gog_alert_add_response (alert, "cancel", "Cancel Install", ADW_RESPONSE_DESTRUCTIVE);
  gog_alert_add_response (alert, "continue", paused ? "Continue" : "Keep Installing",
                          ADW_RESPONSE_SUGGESTED);
  gog_alert_set_default_response (alert, "continue");
  /* Closing it must not throw the install away without asking. */
  gog_alert_set_close_response (alert, "continue");
  gog_alert_present (alert, ctx->window, on_fonts_response, ctx);
}

static void
on_job_log_line (GogInstallJob *job, const gchar *line, gpointer user_data)
{
  (void) job;
  append_log_line (user_data, line);
}

/* A rescan during the install replaced the library's game objects, so
 * ctx->game may no longer be the one the library shows. Give the current
 * object for the same game (same id and folder) the new install record. */
static void
sync_rescanned_copy (InstallCtx *ctx)
{
  GListModel *model = gog_library_get_model (ctx->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      if (g != ctx->game && gog_game_get_id (g) == gog_game_get_id (ctx->game) &&
          g_strcmp0 (gog_game_get_folder_path (g), gog_game_get_folder_path (ctx->game)) == 0)
        gog_state_apply_to_game (ctx->state, g);
      g_object_unref (g);
    }
}

/* Tells screen readers how the install ended; the bar alone doesn't. */
static void
announce (InstallCtx *ctx, const char *message)
{
#if GTK_CHECK_VERSION(4, 14, 0)
  gtk_accessible_announce (GTK_ACCESSIBLE (ctx->window), message,
                           GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
#else
  (void) ctx;
  (void) message;
#endif
}

static void
on_job_finished (GogInstallJob *job, gboolean success, const gchar *error_message, gpointer user_data)
{
  InstallCtx *ctx = user_data;
  (void) job;

  ctx->succeeded = success;
  ctx->finished = TRUE;

  if (success)
    {
      const char *install_dir = gog_install_job_get_install_dir (ctx->job);
      gog_game_set_install_path (ctx->game, install_dir);
      /* Launching needs to know how the game was installed. */
      if (ctx->installer->os == GOG_OS_LINUX || !ctx->chosen_runner)
        {
          gog_game_set_runner_kind (ctx->game, GOG_RUNNER_NATIVE);
          gog_game_set_runner_path (ctx->game, NULL);
          gog_game_set_prefix_path (ctx->game, NULL);
        }
      else
        {
          gchar *prefix = runner_prefix_dir_for_install (ctx->chosen_runner->kind, install_dir);
          gog_game_set_runner_kind (ctx->game, ctx->chosen_runner->kind);
          gog_game_set_runner_path (ctx->game, ctx->chosen_runner->bin_path);
          gog_game_set_prefix_path (ctx->game, prefix);
          g_free (prefix);
        }
      gog_game_set_installed (ctx->game, TRUE);
      gog_state_update_from_game (ctx->state, ctx->game);
      sync_rescanned_copy (ctx);
      GError *save_err = NULL;
      if (!gog_state_save (ctx->state, &save_err))
        {
          append_log_line (ctx, "Warning: could not save install state:");
          append_log_line (ctx, save_err->message);
          g_clear_error (&save_err);
        }
      gtk_progress_bar_set_fraction (ctx->progress_bar, 1.0);
      gtk_progress_bar_set_text (ctx->progress_bar, "Installed");
      append_log_line (ctx, "Install finished successfully.");
      announce (ctx, "Install finished successfully.");
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Failed");
      append_log_line (ctx, error_message ? error_message : "Install failed.");
      gchar *said = g_strdup_printf ("Install failed: %s",
                                     error_message ? error_message : "unknown error");
      announce (ctx, said);
      g_free (said);
    }

  gtk_button_set_label (ctx->action_button, "Close");
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), FALSE);
  gog_dialog_window_set_can_close (ctx->window, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->action_button));
}

static void
populate_runner_dropdown (InstallCtx *ctx)
{
  /* Pre-select the base game's runner for a DLC (it has to share the base
   * game's prefix), otherwise the default runner from Preferences. */
  GogGame *base = gog_game_get_base_game (ctx->game);
  GogRunnerKind want_kind = ctx->config->default_runner_kind;
  const char *want_path = ctx->config->default_runner_path;
  if (ctx->runner_locked)
    {
      want_kind = gog_game_get_runner_kind (base);
      want_path = gog_game_get_runner_path (base);
    }

  GtkStringList *list = gtk_string_list_new (NULL);
  if (ctx->runners)
    g_ptr_array_unref (ctx->runners);
  ctx->runners = runner_discover_all (ctx->config);
  guint select_idx = G_MAXUINT;
  if (ctx->runner_locked && want_path)
    {
      /* Keep the base game's runner available even if discovery no longer
       * finds it, rather than silently switching the DLC to another one. */
      gboolean listed = FALSE;
      for (guint i = 0; i < ctx->runners->len; i++)
        {
          GogRunnerInfo *r = g_ptr_array_index (ctx->runners, i);
          listed = listed || (r->kind == want_kind && g_strcmp0 (r->bin_path, want_path) == 0);
        }
      if (!listed)
        {
          gchar *name = g_path_get_basename (want_path);
          g_ptr_array_add (ctx->runners, gog_runner_info_new (want_kind, name, want_path));
          g_free (name);
        }
    }
  for (guint i = 0; i < ctx->runners->len; i++)
    {
      GogRunnerInfo *r = g_ptr_array_index (ctx->runners, i);
      gchar *label = g_strdup_printf ("%s (%s)", r->name,
                                       r->kind == GOG_RUNNER_PROTON ? "Proton" : "Wine");
      gtk_string_list_append (list, label);
      g_free (label);
      if (select_idx == G_MAXUINT && r->kind == want_kind && g_strcmp0 (r->bin_path, want_path) == 0)
        select_idx = i;
    }
  if (select_idx == G_MAXUINT)
    select_idx = 0;
  gtk_drop_down_set_model (ctx->runner_dropdown, G_LIST_MODEL (list));
  g_object_unref (list);
  if (ctx->runners->len > 0)
    gtk_drop_down_set_selected (ctx->runner_dropdown, select_idx);

  gtk_widget_set_sensitive (GTK_WIDGET (ctx->runner_dropdown),
                            ctx->runners->len > 0 && !ctx->runner_locked);
  /* A Windows installer can't run without Wine or Proton. */
  if (ctx->action_button && !ctx->job)
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), ctx->runners->len > 0);
}

/* user_data is the install window, reffed: the download window can close
 * after it (both go when the main window does), so look ctx up again. */
static void
on_proton_fetch_done (gboolean success, const char *result, gpointer user_data)
{
  (void) result;
  InstallCtx *ctx = g_object_get_data (G_OBJECT (user_data), "install-ctx");
  if (ctx && success)
    populate_runner_dropdown (ctx);
  g_object_unref (user_data);
}

static void
on_get_proton_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  InstallCtx *ctx = user_data;
  gog_show_proton_fetch_dialog (ctx->window, ctx->config, ctx->library, on_proton_fetch_done,
                                g_object_ref (ctx->window));
}

/* TRUE if path is a folder with anything in it. */
static gboolean
dir_has_entries (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (!d)
    return FALSE;
  gboolean any = g_dir_read_name (d) != NULL;
  g_dir_close (d);
  return any;
}

static void
on_choose_dir_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GError *error = NULL;
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, &error);
  /* Cancelled because the install window closed: ctx is gone. */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_error_free (error);
      return;
    }
  g_clear_error (&error);
  InstallCtx *ctx = user_data;
  g_clear_object (&ctx->choosing);
  if (!folder)
    return;
  gchar *path = g_file_get_path (folder);
  g_object_unref (folder);
  if (!path)
    return;
  /* A folder that already holds other things gets the game in a folder of
   * its own, so uninstalling it later can't take those things along. */
  if (dir_has_entries (path) && g_strcmp0 (path, ctx->install_dir) != 0)
    {
      gchar *sub = g_build_filename (path, gog_game_get_slug (ctx->game), NULL);
      g_free (path);
      path = sub;
    }
  g_free (ctx->install_dir);
  ctx->install_dir = path;
  gtk_label_set_label (ctx->install_dir_label, ctx->install_dir);
}

static void
on_choose_dir_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  InstallCtx *ctx = user_data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose Install Directory");
  if (ctx->choosing)
    g_cancellable_cancel (ctx->choosing);
  g_clear_object (&ctx->choosing);
  ctx->choosing = g_cancellable_new ();
  gtk_file_dialog_select_folder (dialog, ctx->window, ctx->choosing, on_choose_dir_response, ctx);
  g_object_unref (dialog);
}

static void
start_install (InstallCtx *ctx)
{
  GogRunnerInfo *runner = NULL;
  if (ctx->installer->os == GOG_OS_WINDOWS)
    {
      guint idx = gtk_drop_down_get_selected (ctx->runner_dropdown);
      if (ctx->runners && idx != GTK_INVALID_LIST_POSITION && idx < ctx->runners->len)
        runner = g_ptr_array_index (ctx->runners, idx);
      if (!runner)
        return;
      ctx->chosen_runner = gog_runner_info_new (runner->kind, runner->name, runner->bin_path);
    }

  ctx->job = gog_install_job_new (ctx->game, ctx->installer, ctx->install_dir, runner);
  g_signal_connect (ctx->job, "progress", G_CALLBACK (on_job_progress), ctx);
  g_signal_connect (ctx->job, "log-line", G_CALLBACK (on_job_log_line), ctx);
  g_signal_connect (ctx->job, "finished", G_CALLBACK (on_job_finished), ctx);
  g_signal_connect (ctx->job, "fonts-missing", G_CALLBACK (on_job_fonts_missing), ctx);

  gtk_widget_set_visible (ctx->setup_box, FALSE);
  gtk_widget_set_visible (ctx->progress_box, TRUE);
  gtk_button_set_label (ctx->action_button, "Installing…");
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), FALSE);
  /* Closing mid-install would orphan the job; use Cancel instead. */
  gog_dialog_window_set_can_close (ctx->window, FALSE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->cancel_button));

  gog_install_job_start (ctx->job);
}

static void
on_action_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  InstallCtx *ctx = user_data;

  if (!ctx->job)
    {
      start_install (ctx);
      return;
    }

  /* Job already finished (button now reads "Close"); on_done runs from
   * on_window_destroy so the title bar's close button reports back too. */
  gtk_window_close (ctx->window);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  InstallCtx *ctx = user_data;
  if (ctx->job)
    gog_install_job_cancel (ctx->job);
  else
    gtk_window_close (ctx->window);
}

static void
on_window_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  InstallCtx *ctx = user_data;
  if (ctx->on_done)
    ctx->on_done (ctx->game, ctx->succeeded, ctx->user_data);
  ctx_free (ctx);
}

void
gog_show_install_dialog (GtkWindow *parent, GogGame *game, GogInstallerInfo *installer,
                          GogConfig *config, GogLibrary *library, GogState *state,
                          GogInstallDoneFunc on_done, gpointer user_data)
{
  InstallCtx *ctx = g_new0 (InstallCtx, 1);
  ctx->parent = parent;
  ctx->game = g_object_ref (game);
  ctx->installer = installer;
  ctx->config = config;
  ctx->library = library;
  ctx->state = state;
  ctx->on_done = on_done;
  ctx->user_data = user_data;

  GogGame *base = gog_game_get_base_game (game);
  gboolean into_base = base && gog_game_get_installed (base) && gog_game_get_install_path (base);
  if (into_base)
    ctx->install_dir = g_strdup (gog_game_get_install_path (base));
  else if (config->install_root)
    ctx->install_dir = g_build_filename (config->install_root, gog_game_get_slug (game), NULL);
  else
    ctx->install_dir = g_build_filename (g_get_home_dir (), "GOG Games", gog_game_get_slug (game), NULL);
  ctx->runner_locked = into_base && (gog_game_get_runner_kind (base) == GOG_RUNNER_WINE ||
                                     gog_game_get_runner_kind (base) == GOG_RUNNER_PROTON);

  ctx->window = gog_dialog_window_new (parent, "Install", 560, -1, 360, 240);
  g_object_set_data (G_OBJECT (ctx->window), "install-ctx", ctx);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), TRUE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (outer, 18);
  gtk_widget_set_margin_end (outer, 18);
  gtk_widget_set_margin_top (outer, 12);
  gtk_widget_set_margin_bottom (outer, 18);

  gchar *heading = g_strdup_printf ("Install %s", gog_game_get_title (game));
  GtkWidget *title_label = gtk_label_new (heading);
  gtk_widget_add_css_class (title_label, "title-2");
  gtk_widget_set_halign (title_label, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (outer), title_label);
  g_free (heading);

  /* --- setup box --- */
  ctx->setup_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);

  if (installer->os == GOG_OS_WINDOWS)
    {
      GtkWidget *runner_row = adw_action_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (runner_row), "Runner");
      ctx->runner_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
      gtk_widget_set_valign (GTK_WIDGET (ctx->runner_dropdown), GTK_ALIGN_CENTER);
      gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->runner_dropdown),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, "Runner", -1);
      adw_action_row_add_suffix (ADW_ACTION_ROW (runner_row), GTK_WIDGET (ctx->runner_dropdown));
      GtkWidget *get_proton_btn = gtk_button_new_from_icon_name ("software-update-available-symbolic");
      gtk_widget_set_valign (get_proton_btn, GTK_ALIGN_CENTER);
      gtk_widget_set_tooltip_text (get_proton_btn, "Download the latest GE-Proton");
      gtk_accessible_update_property (GTK_ACCESSIBLE (get_proton_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Download the latest GE-Proton", -1);
      gtk_widget_set_sensitive (get_proton_btn, !ctx->runner_locked);
      g_signal_connect (get_proton_btn, "clicked", G_CALLBACK (on_get_proton_clicked), ctx);
      adw_action_row_add_suffix (ADW_ACTION_ROW (runner_row), get_proton_btn);
      GtkWidget *runner_group = adw_preferences_group_new ();
      adw_preferences_group_add (ADW_PREFERENCES_GROUP (runner_group), runner_row);
      gtk_box_append (GTK_BOX (ctx->setup_box), runner_group);
    }
  else
    {
      GtkWidget *info = gtk_label_new ("This is a native Linux installer — no Wine/Proton needed.");
      gtk_widget_set_halign (info, GTK_ALIGN_START);
      gtk_widget_add_css_class (info, "dim-label");
      gtk_box_append (GTK_BOX (ctx->setup_box), info);
    }

  GtkWidget *dir_row = adw_action_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dir_row), "Install to");
  ctx->install_dir_label = GTK_LABEL (gtk_label_new (ctx->install_dir));
  gtk_label_set_ellipsize (ctx->install_dir_label, PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (ctx->install_dir_label, 30);
  adw_action_row_add_suffix (ADW_ACTION_ROW (dir_row), GTK_WIDGET (ctx->install_dir_label));
  GtkWidget *choose_btn = gtk_button_new_from_icon_name ("folder-open-symbolic");
  gtk_widget_set_valign (choose_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text (choose_btn, "Choose install directory");
  gtk_accessible_update_property (GTK_ACCESSIBLE (choose_btn), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Choose install directory", -1);
  g_signal_connect (choose_btn, "clicked", G_CALLBACK (on_choose_dir_clicked), ctx);
  adw_action_row_add_suffix (ADW_ACTION_ROW (dir_row), choose_btn);

  if (into_base)
    gtk_widget_set_sensitive (choose_btn, FALSE); /* DLC installs into the base game's dir */

  GtkWidget *dir_group = adw_preferences_group_new ();
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (dir_group), dir_row);
  gtk_box_append (GTK_BOX (ctx->setup_box), dir_group);

  gtk_box_append (GTK_BOX (outer), ctx->setup_box);

  /* --- progress box (hidden until Install is clicked) --- */
  ctx->progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_visible (ctx->progress_box, FALSE);

  ctx->progress_bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->progress_bar),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Install progress", -1);
  gtk_progress_bar_set_show_text (ctx->progress_bar, TRUE);
  gtk_box_append (GTK_BOX (ctx->progress_box), GTK_WIDGET (ctx->progress_bar));

  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (log_scroll, -1, 220);
  /* Resizing the window gives the log the room. */
  gtk_widget_set_vexpand (log_scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (log_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  ctx->log_view = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->log_view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Install log", -1);
  gtk_text_view_set_editable (ctx->log_view, FALSE);
  gtk_text_view_set_monospace (ctx->log_view, TRUE);
  /* Long lines (installer paths, the FreeType fix commands) wrap rather
   * than widen the dialog. */
  gtk_text_view_set_wrap_mode (ctx->log_view, GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->log_view), "gog-log-view");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), GTK_WIDGET (ctx->log_view));
  gtk_widget_add_css_class (log_scroll, "card");
  gtk_box_append (GTK_BOX (ctx->progress_box), log_scroll);

  gtk_box_append (GTK_BOX (outer), ctx->progress_box);

  /* --- action buttons --- */
  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  ctx->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  g_signal_connect (ctx->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->action_button = GTK_BUTTON (gtk_button_new_with_label ("Install"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->action_button), "suggested-action");
  g_signal_connect (ctx->action_button, "clicked", G_CALLBACK (on_action_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->cancel_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->action_button));
  gtk_box_append (GTK_BOX (outer), btn_box);

  if (ctx->runner_dropdown)
    populate_runner_dropdown (ctx);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), outer);
  adw_window_set_content (ADW_WINDOW (ctx->window), toolbar_view);

  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_window_destroy), ctx);

  gtk_window_present (ctx->window);
}
