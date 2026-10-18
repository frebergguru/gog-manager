/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gog_sync_dialog.h"
#include "../gog_sync.h"

typedef struct
{
  AdwDialog *dialog;
  gchar *library_dir;
  GogLibrary *library;
  GogSyncDoneFunc on_done;
  gpointer user_data;

  GtkLabel *summary_label;
  GtkWidget *results_scroll;
  GtkWidget *results_box;
  GtkWidget *progress_box;
  GtkProgressBar *progress_bar;
  GtkTextView *log_view;
  GtkButton *check_button;
  GtkButton *select_all_button;
  GtkButton *download_button;
  GtkButton *cancel_button;

  GPtrArray *items;  /* GogSyncItem*, what is listed */
  GPtrArray *checks; /* GtkCheckButton*, parallel to items */

  GogSyncJob *job;
  gboolean downloaded;
} SyncCtx;

static void
ctx_free (SyncCtx *ctx)
{
  if (ctx->job)
    {
      /* The job's queued idles may still fire after the dialog is gone. */
      g_signal_handlers_disconnect_by_data (ctx->job, ctx);
      gog_sync_job_cancel (ctx->job);
      g_clear_object (&ctx->job);
    }
  g_ptr_array_unref (ctx->items);
  g_ptr_array_unref (ctx->checks);
  g_free (ctx->library_dir);
  g_free (ctx);
}

static void
announce (SyncCtx *ctx, const char *message)
{
#if GTK_CHECK_VERSION(4, 14, 0)
  gtk_accessible_announce (GTK_ACCESSIBLE (ctx->dialog), message,
                           GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
#else
  (void) ctx;
  (void) message;
#endif
}

static void
append_log_line (SyncCtx *ctx, const char *line)
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

/* The library's title for a downloaded game, else one made from its name. */
static gchar *
title_for (SyncCtx *ctx, const char *gamename)
{
  GListModel *model = gog_library_get_model (ctx->library);
  guint n = g_list_model_get_n_items (model);
  gchar *title = NULL;
  for (guint i = 0; i < n && !title; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      gchar *folder = gog_game_get_folder_path (g) ? g_path_get_basename (gog_game_get_folder_path (g))
                                                   : NULL;
      if (g_strcmp0 (folder, gamename) == 0)
        title = g_strdup (gog_game_get_title (g));
      g_free (folder);
      g_object_unref (g);
    }
  return title ? title : gog_sync_pretty_name (gamename);
}

static gboolean
any_checked (SyncCtx *ctx)
{
  for (guint i = 0; i < ctx->checks->len; i++)
    if (gtk_check_button_get_active (g_ptr_array_index (ctx->checks, i)))
      return TRUE;
  return FALSE;
}

static void
set_busy (SyncCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->check_button), !busy);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->select_all_button), !busy && ctx->items->len > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), !busy && any_checked (ctx));
  gtk_widget_set_sensitive (ctx->results_box, !busy);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), busy);
  /* Closing mid-run would orphan the job; use Cancel instead. */
  adw_dialog_set_can_close (ctx->dialog, !busy);
}

static void
on_check_toggled (GtkCheckButton *check, gpointer user_data)
{
  (void) check;
  SyncCtx *ctx = user_data;
  if (!ctx->job)
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), any_checked (ctx));
}

static void
add_group (SyncCtx *ctx, GogSyncKind kind, const char *heading)
{
  GtkWidget *list = NULL;
  for (guint i = 0; i < ctx->items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (ctx->items, i);
      if (item->kind != kind)
        continue;
      if (!list)
        {
          GtkWidget *label = gtk_label_new (heading);
          gtk_widget_add_css_class (label, "heading");
          gtk_label_set_xalign (GTK_LABEL (label), 0);
          gtk_box_append (GTK_BOX (ctx->results_box), label);
          list = gtk_list_box_new ();
          gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
          gtk_widget_add_css_class (list, "boxed-list");
          gtk_accessible_update_property (GTK_ACCESSIBLE (list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          heading, -1);
          gtk_box_append (GTK_BOX (ctx->results_box), list);
        }

      gchar *title = title_for (ctx, item->gamename);
      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      gchar *subtitle = g_strdup_printf (item->n_files == 1 ? "%u file to download"
                                                            : "%u files to download",
                                         item->n_files);
      adw_action_row_set_subtitle (row, subtitle);

      GtkWidget *check = gtk_check_button_new ();
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      gchar *label = g_strdup_printf ("Select %s", title);
      gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label, -1);
      g_signal_connect (check, "toggled", G_CALLBACK (on_check_toggled), ctx);
      adw_action_row_add_prefix (row, check);
      adw_action_row_set_activatable_widget (row, check);
      g_ptr_array_add (ctx->checks, check);
      gtk_list_box_append (GTK_LIST_BOX (list), GTK_WIDGET (row));

      g_free (label);
      g_free (subtitle);
      g_free (title);
    }
}

/* Takes ownership of items. */
static void
show_items (SyncCtx *ctx, GPtrArray *items, gint64 checked_unix)
{
  g_ptr_array_unref (ctx->items);
  ctx->items = items;
  g_ptr_array_set_size (ctx->checks, 0);
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (ctx->results_box)))
    gtk_box_remove (GTK_BOX (ctx->results_box), child);

  guint n_new = 0, n_update = 0;
  for (guint i = 0; i < items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (items, i);
      if (item->kind == GOG_SYNC_NEW_GAME)
        n_new++;
      else
        n_update++;
    }
  add_group (ctx, GOG_SYNC_NEW_GAME, "Not Downloaded Yet");
  add_group (ctx, GOG_SYNC_UPDATE, "New Versions");

  gchar *when = NULL;
  if (checked_unix > 0)
    {
      GDateTime *dt = g_date_time_new_from_unix_local (checked_unix);
      when = g_date_time_format (dt, "%x %H:%M");
      g_date_time_unref (dt);
    }
  gchar *summary;
  if (!when)
    summary = g_strdup ("GOG has not been checked yet. Checking looks at every game you own "
                        "and takes a few minutes.");
  else if (items->len == 0)
    summary = g_strdup_printf ("Last checked %s: everything is downloaded and up to date.", when);
  else
    summary = g_strdup_printf ("Last checked %s: %u not downloaded yet, %u with a new version. "
                               "Choose what to download; old versions are deleted afterwards.",
                               when, n_new, n_update);
  gtk_label_set_text (ctx->summary_label, summary);
  gtk_widget_set_visible (ctx->results_scroll, items->len > 0);
  g_free (summary);
  g_free (when);
}

static void
on_job_progress (GogSyncJob *job, gdouble fraction, const gchar *message, gpointer user_data)
{
  (void) job;
  SyncCtx *ctx = user_data;
  if (fraction < 0)
    gtk_progress_bar_pulse (ctx->progress_bar);
  else
    gtk_progress_bar_set_fraction (ctx->progress_bar, fraction);
  if (message)
    gtk_progress_bar_set_text (ctx->progress_bar, message);
}

static void
on_job_log_line (GogSyncJob *job, const gchar *line, gpointer user_data)
{
  (void) job;
  append_log_line (user_data, line);
}

static void start_job (SyncCtx *ctx, GogSyncJob *job);

static void
on_check_finished (GogSyncJob *job, gboolean success, const gchar *error_message,
                   gpointer user_data)
{
  SyncCtx *ctx = user_data;
  if (success)
    {
      GPtrArray *items = g_ptr_array_ref (gog_sync_job_get_items (job));
      gtk_progress_bar_set_fraction (ctx->progress_bar, 1.0);
      gtk_progress_bar_set_text (ctx->progress_bar, "Check finished");
      show_items (ctx, items, g_get_real_time () / G_USEC_PER_SEC);
      announce (ctx, gtk_label_get_text (ctx->summary_label));
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Check failed");
      append_log_line (ctx, error_message ? error_message : "The check failed.");
      announce (ctx, error_message ? error_message : "The check failed.");
    }
  g_signal_handlers_disconnect_by_data (job, ctx);
  g_clear_object (&ctx->job);
  set_busy (ctx, FALSE);
}

static void
on_download_finished (GogSyncJob *job, gboolean success, const gchar *error_message,
                      gpointer user_data)
{
  SyncCtx *ctx = user_data;
  /* Even a failed or cancelled run may have fetched some games. */
  ctx->downloaded = TRUE;
  const char *msg = success ? "Download finished. Old versions were deleted." : NULL;
  if (success)
    {
      gtk_progress_bar_set_fraction (ctx->progress_bar, 1.0);
      gtk_progress_bar_set_text (ctx->progress_bar, "Download finished");
      append_log_line (ctx, msg);
      announce (ctx, msg);
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Download stopped");
      append_log_line (ctx, error_message ? error_message : "The download failed.");
      announce (ctx, error_message ? error_message : "The download failed.");
    }
  g_signal_handlers_disconnect_by_data (job, ctx);
  g_clear_object (&ctx->job);

  gint64 checked = 0;
  show_items (ctx, gog_sync_load_results (&checked), checked);
  set_busy (ctx, FALSE);
}

static void
start_job (SyncCtx *ctx, GogSyncJob *job)
{
  ctx->job = job;
  g_signal_connect (job, "progress", G_CALLBACK (on_job_progress), ctx);
  g_signal_connect (job, "log-line", G_CALLBACK (on_job_log_line), ctx);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (ctx->log_view), "", -1);
  gtk_progress_bar_set_fraction (ctx->progress_bar, 0);
  gtk_widget_set_visible (ctx->progress_box, TRUE);
  set_busy (ctx, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->cancel_button));
  gog_sync_job_start (job);
}

static void
on_check_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  GogSyncJob *job = gog_sync_job_new_check (ctx->library_dir);
  g_signal_connect (job, "finished", G_CALLBACK (on_check_finished), ctx);
  start_job (ctx, job);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  GStrvBuilder *builder = g_strv_builder_new ();
  for (guint i = 0; i < ctx->checks->len; i++)
    if (gtk_check_button_get_active (g_ptr_array_index (ctx->checks, i)))
      g_strv_builder_add (builder, ((GogSyncItem *) g_ptr_array_index (ctx->items, i))->gamename);
  GStrv names = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  if (names[0])
    {
      GogSyncJob *job = gog_sync_job_new_download (ctx->library_dir, (const char *const *) names);
      g_signal_connect (job, "finished", G_CALLBACK (on_download_finished), ctx);
      start_job (ctx, job);
    }
  g_strfreev (names);
}

static void
on_select_all_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  for (guint i = 0; i < ctx->checks->len; i++)
    gtk_check_button_set_active (g_ptr_array_index (ctx->checks, i), TRUE);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  if (ctx->job)
    gog_sync_job_cancel (ctx->job);
}

static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  SyncCtx *ctx = user_data;
  if (ctx->on_done)
    ctx->on_done (ctx->downloaded, ctx->user_data);
  ctx_free (ctx);
}

void
gog_show_sync_dialog (GtkWindow *parent, const char *library_dir, GogLibrary *library,
                      GogSyncDoneFunc on_done, gpointer user_data)
{
  SyncCtx *ctx = g_new0 (SyncCtx, 1);
  ctx->library_dir = g_strdup (library_dir);
  ctx->library = library;
  ctx->on_done = on_done;
  ctx->user_data = user_data;
  ctx->items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);
  ctx->checks = g_ptr_array_new ();

  AdwDialog *dialog = adw_dialog_new ();
  ctx->dialog = dialog;
  adw_dialog_set_title (dialog, "New Games and Updates on GOG");
  adw_dialog_set_content_width (dialog, 560);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), adw_header_bar_new ());

  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (outer, 18);
  gtk_widget_set_margin_end (outer, 18);
  gtk_widget_set_margin_top (outer, 12);
  gtk_widget_set_margin_bottom (outer, 18);

  ctx->summary_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (ctx->summary_label, TRUE);
  gtk_label_set_xalign (ctx->summary_label, 0);
  gtk_box_append (GTK_BOX (outer), GTK_WIDGET (ctx->summary_label));

  ctx->results_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  ctx->results_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (ctx->results_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (ctx->results_scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (ctx->results_scroll), 360);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (ctx->results_scroll), ctx->results_box);
  gtk_box_append (GTK_BOX (outer), ctx->results_scroll);

  ctx->progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_visible (ctx->progress_box, FALSE);
  ctx->progress_bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_progress_bar_set_show_text (ctx->progress_bar, TRUE);
  gtk_progress_bar_set_ellipsize (ctx->progress_bar, PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append (GTK_BOX (ctx->progress_box), GTK_WIDGET (ctx->progress_bar));
  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (log_scroll, -1, 140);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (log_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  ctx->log_view = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_editable (ctx->log_view, FALSE);
  gtk_text_view_set_monospace (ctx->log_view, TRUE);
  gtk_text_view_set_wrap_mode (ctx->log_view, GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->log_view), "gog-log-view");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->log_view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "lgogdownloader output", -1);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), GTK_WIDGET (ctx->log_view));
  gtk_widget_add_css_class (log_scroll, "card");
  gtk_box_append (GTK_BOX (ctx->progress_box), log_scroll);
  gtk_box_append (GTK_BOX (outer), ctx->progress_box);

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  ctx->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), FALSE);
  g_signal_connect (ctx->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->select_all_button = GTK_BUTTON (gtk_button_new_with_label ("Select All"));
  g_signal_connect (ctx->select_all_button, "clicked", G_CALLBACK (on_select_all_clicked), ctx);
  ctx->check_button = GTK_BUTTON (gtk_button_new_with_label ("Check Now"));
  g_signal_connect (ctx->check_button, "clicked", G_CALLBACK (on_check_clicked), ctx);
  ctx->download_button = GTK_BUTTON (gtk_button_new_with_label ("Download Selected"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->download_button), "suggested-action");
  g_signal_connect (ctx->download_button, "clicked", G_CALLBACK (on_download_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->cancel_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->select_all_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->check_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->download_button));
  gtk_box_append (GTK_BOX (outer), btn_box);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), outer);
  adw_dialog_set_child (dialog, toolbar_view);

  if (gog_sync_is_available ())
    {
      gint64 checked = 0;
      show_items (ctx, gog_sync_load_results (&checked), checked);
      set_busy (ctx, FALSE);
    }
  else
    {
      gtk_label_set_text (ctx->summary_label,
                          "This needs lgogdownloader, which was not found. Install it from your "
                          "distribution's packages, run \"lgogdownloader --login\" once in a "
                          "terminal, and open this again.");
      gtk_widget_set_visible (ctx->results_scroll, FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->check_button), FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->select_all_button), FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), FALSE);
    }

  g_signal_connect (dialog, "closed", G_CALLBACK (on_dialog_closed), ctx);
  adw_dialog_present (dialog, GTK_WIDGET (parent));
}
