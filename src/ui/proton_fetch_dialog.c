/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "proton_fetch_dialog.h"
#include "../proton_fetch.h"

typedef struct
{
  AdwDialog *dialog;
  GtkWindow *parent;
  GogProtonFetchDoneFunc on_done;
  gpointer user_data;

  GtkWidget *intro_box;
  GtkWidget *progress_box;
  GtkProgressBar *progress_bar;
  GtkTextView *log_view;
  GtkButton *action_button; /* Download -> Close */
  GtkButton *cancel_button;

  GogProtonFetchJob *job;
  gboolean succeeded;
  gchar *result;
} FetchCtx;

static void
ctx_free (FetchCtx *ctx)
{
  g_clear_object (&ctx->job);
  g_free (ctx->result);
  g_free (ctx);
}

static void
append_log_line (FetchCtx *ctx, const char *line)
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
on_job_progress (GogProtonFetchJob *job, gdouble fraction, const gchar *message, gpointer user_data)
{
  (void) job;
  FetchCtx *ctx = user_data;
  if (fraction < 0)
    gtk_progress_bar_pulse (ctx->progress_bar);
  else
    gtk_progress_bar_set_fraction (ctx->progress_bar, fraction);
  if (message)
    gtk_progress_bar_set_text (ctx->progress_bar, message);
}

static void
on_job_log_line (GogProtonFetchJob *job, const gchar *line, gpointer user_data)
{
  (void) job;
  append_log_line (user_data, line);
}

static void
on_job_finished (GogProtonFetchJob *job, gboolean success, const gchar *result, gpointer user_data)
{
  (void) job;
  FetchCtx *ctx = user_data;
  ctx->succeeded = success;
  g_free (ctx->result);
  ctx->result = g_strdup (result);

  if (success)
    {
      gchar *msg = g_strdup_printf ("%s is ready to use", result);
      gtk_progress_bar_set_fraction (ctx->progress_bar, 1.0);
      gtk_progress_bar_set_text (ctx->progress_bar, msg);
      append_log_line (ctx, msg);
      g_free (msg);
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Failed");
      append_log_line (ctx, result ? result : "Download failed.");
    }

  gtk_button_set_label (ctx->action_button, "Close");
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), FALSE);
}

static void
start_download (FetchCtx *ctx)
{
  ctx->job = gog_proton_fetch_job_new ();
  g_signal_connect (ctx->job, "progress", G_CALLBACK (on_job_progress), ctx);
  g_signal_connect (ctx->job, "log-line", G_CALLBACK (on_job_log_line), ctx);
  g_signal_connect (ctx->job, "finished", G_CALLBACK (on_job_finished), ctx);

  gtk_widget_set_visible (ctx->intro_box, FALSE);
  gtk_widget_set_visible (ctx->progress_box, TRUE);
  gtk_button_set_label (ctx->action_button, "Downloading…");
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), FALSE);

  gog_proton_fetch_job_start (ctx->job);
}

static void
on_action_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FetchCtx *ctx = user_data;
  if (!ctx->job)
    {
      start_download (ctx);
      return;
    }
  adw_dialog_close (ctx->dialog);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FetchCtx *ctx = user_data;
  if (ctx->job)
    gog_proton_fetch_job_cancel (ctx->job);
  else
    adw_dialog_close (ctx->dialog);
}

static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  FetchCtx *ctx = user_data;
  if (ctx->on_done)
    ctx->on_done (ctx->succeeded, ctx->result, ctx->user_data);
  ctx_free (ctx);
}

void
gog_show_proton_fetch_dialog (GtkWindow *parent, GogProtonFetchDoneFunc on_done, gpointer user_data)
{
  FetchCtx *ctx = g_new0 (FetchCtx, 1);
  ctx->parent = parent;
  ctx->on_done = on_done;
  ctx->user_data = user_data;

  AdwDialog *dialog = adw_dialog_new ();
  ctx->dialog = dialog;
  adw_dialog_set_title (dialog, "Download GE-Proton");
  adw_dialog_set_content_width (dialog, 480);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), TRUE);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (outer, 18);
  gtk_widget_set_margin_end (outer, 18);
  gtk_widget_set_margin_top (outer, 12);
  gtk_widget_set_margin_bottom (outer, 18);

  ctx->intro_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *title = gtk_label_new ("Download the latest GE-Proton");
  gtk_widget_add_css_class (title, "title-2");
  gtk_widget_set_halign (title, GTK_ALIGN_START);
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_box_append (GTK_BOX (ctx->intro_box), title);

  GtkWidget *desc = gtk_label_new (
      "Fetches the newest release from GloriousEggroll/proton-ge-custom on GitHub "
      "(a few hundred MB) and installs it for use with Windows games. Already-downloaded "
      "versions are detected and skipped.");
  gtk_label_set_wrap (GTK_LABEL (desc), TRUE);
  gtk_widget_set_halign (desc, GTK_ALIGN_START);
  gtk_widget_add_css_class (desc, "dim-label");
  gtk_box_append (GTK_BOX (ctx->intro_box), desc);
  gtk_box_append (GTK_BOX (outer), ctx->intro_box);

  ctx->progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_visible (ctx->progress_box, FALSE);

  ctx->progress_bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_progress_bar_set_show_text (ctx->progress_bar, TRUE);
  gtk_box_append (GTK_BOX (ctx->progress_box), GTK_WIDGET (ctx->progress_bar));

  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_widget_set_size_request (log_scroll, -1, 160);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (log_scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
  ctx->log_view = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_editable (ctx->log_view, FALSE);
  gtk_text_view_set_monospace (ctx->log_view, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->log_view), "gog-log-view");
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), GTK_WIDGET (ctx->log_view));
  gtk_widget_add_css_class (log_scroll, "card");
  gtk_box_append (GTK_BOX (ctx->progress_box), log_scroll);

  gtk_box_append (GTK_BOX (outer), ctx->progress_box);

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  ctx->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  g_signal_connect (ctx->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->action_button = GTK_BUTTON (gtk_button_new_with_label ("Download"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->action_button), "suggested-action");
  g_signal_connect (ctx->action_button, "clicked", G_CALLBACK (on_action_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->cancel_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->action_button));
  gtk_box_append (GTK_BOX (outer), btn_box);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), outer);
  adw_dialog_set_child (dialog, toolbar_view);

  g_signal_connect (dialog, "closed", G_CALLBACK (on_dialog_closed), ctx);
  adw_dialog_present (dialog, GTK_WIDGET (parent));
}
