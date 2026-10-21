/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "repair_dialog.h"
#include "dialog_window.h"
#include "../gog_sync.h"

typedef struct
{
  GtkWindow *window;
  GogSyncJob *job;
  GtkLabel *status;
  GtkProgressBar *bar;
  GtkTextView *log;
  GtkWidget *cancel;
  GtkWidget *close;
  guint pulse_id;
  gchar *title;
} RepairCtx;

static void
stop_pulse (RepairCtx *ctx)
{
  if (ctx->pulse_id)
    g_source_remove (ctx->pulse_id);
  ctx->pulse_id = 0;
}

static void
ctx_free (RepairCtx *ctx)
{
  stop_pulse (ctx);
  if (ctx->job)
    {
      /* Its queued signals may still come after the window is gone. */
      g_signal_handlers_disconnect_by_data (ctx->job, ctx);
      gog_sync_job_cancel (ctx->job);
      g_clear_object (&ctx->job);
    }
  g_free (ctx->title);
  g_free (ctx);
}

static gboolean
pulse (gpointer data)
{
  gtk_progress_bar_pulse (((RepairCtx *) data)->bar);
  return G_SOURCE_CONTINUE;
}

static void
on_progress (GogSyncJob *job, gdouble fraction, const gchar *message, gpointer user_data)
{
  (void) job;
  (void) fraction;
  RepairCtx *ctx = user_data;
  gtk_label_set_text (ctx->status, message);
}

static void
on_log_line (GogSyncJob *job, const gchar *line, gpointer user_data)
{
  (void) job;
  RepairCtx *ctx = user_data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (ctx->log);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_insert (buffer, &end, line, -1);
  gtk_text_buffer_insert (buffer, &end, "\n", 1);
  gtk_text_buffer_get_end_iter (buffer, &end);
  GtkTextMark *mark = gtk_text_buffer_create_mark (buffer, NULL, &end, FALSE);
  gtk_text_view_scroll_mark_onscreen (ctx->log, mark);
  gtk_text_buffer_delete_mark (buffer, mark);
}

static void
on_finished (GogSyncJob *job, gboolean success, const gchar *error_message, gpointer user_data)
{
  RepairCtx *ctx = user_data;
  stop_pulse (ctx);
  const char *summary = gog_sync_job_get_summary (job);
  gchar *text = success ? g_strdup (summary ? summary : "Done.")
                        : g_strdup_printf ("The check stopped: %s",
                                           error_message ? error_message : "lgogdownloader failed");
  gtk_label_set_text (ctx->status, text);
  gtk_progress_bar_set_fraction (ctx->bar, success ? 1.0 : 0.0);
  gtk_widget_set_visible (GTK_WIDGET (ctx->bar), FALSE);
  gog_announce (GTK_WIDGET (ctx->window), text);
  g_free (text);
  gtk_widget_set_visible (ctx->cancel, FALSE);
  gtk_widget_set_visible (ctx->close, TRUE);
  gog_dialog_window_set_can_close (ctx->window, TRUE);
  gtk_widget_grab_focus (ctx->close);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  RepairCtx *ctx = user_data;
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_label_set_text (ctx->status, "Stopping…");
  gog_sync_job_cancel (ctx->job);
}

static void
on_close_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  gtk_window_close (((RepairCtx *) user_data)->window);
}

void
gog_show_repair_dialog (GtkWindow *parent, const char *library_dir, GogGame *game)
{
  RepairCtx *ctx = g_new0 (RepairCtx, 1);
  ctx->title = g_strdup (gog_game_get_title (game));
  gchar *heading = g_strdup_printf ("Check Files: %s", ctx->title);
  ctx->window = gog_dialog_window_new (parent, heading, 620, 460, 360, 300);
  g_free (heading);
  g_object_set_data_full (G_OBJECT (ctx->window), "repair", ctx, (GDestroyNotify) ctx_free);

  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  GtkWidget *intro = gtk_label_new ("lgogdownloader compares the downloaded files with GOG's "
                                    "checksums, part by part, and downloads any damaged part "
                                    "again. Nothing else is downloaded or deleted.");
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_label_set_xalign (GTK_LABEL (intro), 0.0);
  gtk_widget_add_css_class (intro, "dim-label");
  gtk_box_append (GTK_BOX (box), intro);

  ctx->status = GTK_LABEL (gtk_label_new ("Starting…"));
  gtk_label_set_wrap (ctx->status, TRUE);
  gtk_label_set_xalign (ctx->status, 0.0);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->status));

  ctx->bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->bar), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Checking files", -1);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->bar));

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_widget_add_css_class (scroll, "card");
  ctx->log = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_editable (ctx->log, FALSE);
  gtk_text_view_set_monospace (ctx->log, TRUE);
  gtk_text_view_set_wrap_mode (ctx->log, GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->log), "gog-log-view");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->log), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "lgogdownloader output", -1);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (ctx->log));
  gtk_box_append (GTK_BOX (box), scroll);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ctx->cancel = gtk_button_new_with_label ("Cancel");
  g_signal_connect (ctx->cancel, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->close = gtk_button_new_with_label ("Close");
  gtk_widget_add_css_class (ctx->close, "suggested-action");
  gtk_widget_set_visible (ctx->close, FALSE);
  g_signal_connect (ctx->close, "clicked", G_CALLBACK (on_close_clicked), ctx);
  gtk_box_append (GTK_BOX (buttons), ctx->cancel);
  gtk_box_append (GTK_BOX (buttons), ctx->close);
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_window_set_content (ADW_WINDOW (ctx->window), view);

  if (!gog_sync_is_available ())
    {
      gtk_label_set_text (ctx->status, "This needs lgogdownloader, which was not found.");
      gtk_widget_set_visible (GTK_WIDGET (ctx->bar), FALSE);
      gtk_widget_set_visible (ctx->cancel, FALSE);
      gtk_widget_set_visible (ctx->close, TRUE);
      gtk_window_present (ctx->window);
      return;
    }

  /* lgogdownloader's name for it, which is also its folder's (see
   * gog_library_folder_misnamed()). */
  gchar *gamename = gog_game_get_gamename (game) ? g_strdup (gog_game_get_gamename (game))
                                                 : g_path_get_basename (gog_game_get_folder_path (game));
  ctx->job = gog_sync_job_new_repair (library_dir, gamename);
  g_free (gamename);
  g_signal_connect (ctx->job, "progress", G_CALLBACK (on_progress), ctx);
  g_signal_connect (ctx->job, "log-line", G_CALLBACK (on_log_line), ctx);
  g_signal_connect (ctx->job, "finished", G_CALLBACK (on_finished), ctx);
  /* Cancel is the way out while it runs. */
  gog_dialog_window_set_can_close (ctx->window, FALSE);
  /* Reduced motion: a still bar, the text says what's going on. */
  gboolean animate = TRUE;
  g_object_get (gtk_settings_get_default (), "gtk-enable-animations", &animate, NULL);
  if (animate)
    ctx->pulse_id = g_timeout_add (150, pulse, ctx);
  gog_sync_job_start (ctx->job);
  gtk_window_present (ctx->window);
  gtk_widget_grab_focus (ctx->cancel);
}
