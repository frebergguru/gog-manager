/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "proton_fetch_dialog.h"
#include "dialog_window.h"
#include "proton_manage_dialog.h"
#include "../proton_fetch.h"
#include "../text_util.h"

typedef struct
{
  GtkWindow *window;
  GtkWindow *parent;
  GogConfig *config;
  GogLibrary *library;
  GogProtonFetchDoneFunc on_done;
  gpointer user_data;

  GtkWidget *intro_box;
  GtkWidget *progress_box;
  GtkProgressBar *progress_bar;
  GtkTextView *log_view;
  GtkButton *action_button; /* Download -> Close */
  GtkButton *cancel_button;

  GtkWidget *cleanup_box; /* offer to remove unused older versions */
  GtkLabel *cleanup_label;
  GtkButton *cleanup_button;
  GStrv unused;           /* the versions offered */
  GCancellable *measuring;

  GogProtonFetchJob *job;
  gboolean succeeded;
  gchar *result;
} FetchCtx;

static void
ctx_free (FetchCtx *ctx)
{
  if (ctx->job)
    {
      /* The job's queued idles may still fire after the dialog is gone. */
      g_signal_handlers_disconnect_by_data (ctx->job, ctx);
      g_clear_object (&ctx->job);
    }
  if (ctx->measuring)
    {
      g_cancellable_cancel (ctx->measuring);
      g_object_unref (ctx->measuring);
    }
  g_strfreev (ctx->unused);
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
set_cleanup_text (FetchCtx *ctx, guint64 total)
{
  gchar *list = g_strjoinv (", ", ctx->unused);
  guint n = g_strv_length (ctx->unused);
  gchar *text;
  if (total > 0)
    {
      gchar *size_str = text_format_size (total);
      text = g_strdup_printf (n == 1 ? "An older version no game uses takes up %s: %s."
                                     : "Older versions no game uses take up %s: %s.",
                              size_str, list);
      g_free (size_str);
    }
  else
    text = g_strdup_printf (n == 1 ? "An older version no game uses is still here: %s."
                                   : "Older versions no game uses are still here: %s.",
                            list);
  gtk_label_set_text (ctx->cleanup_label, text);
  g_free (text);
  g_free (list);
}

static void
on_unused_measured (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GArray *sizes = proton_fetch_measure_finish (res, NULL);
  if (!sizes)
    return; /* cancelled: the dialog is gone, or the cleanup began */
  FetchCtx *ctx = user_data;
  g_clear_object (&ctx->measuring);
  guint64 total = 0;
  for (guint i = 0; i < sizes->len; i++)
    total += g_array_index (sizes, guint64, i);
  g_array_unref (sizes);
  set_cleanup_text (ctx, total);
}

static void
offer_cleanup (FetchCtx *ctx)
{
  if (!ctx->library || !ctx->config)
    return;
  g_strfreev (ctx->unused);
  ctx->unused = gog_proton_unused_old_versions (ctx->config, ctx->library);
  if (!ctx->unused[0])
    return;
  set_cleanup_text (ctx, 0);
  gtk_button_set_label (ctx->cleanup_button,
                        g_strv_length (ctx->unused) == 1 ? "Remove It" : "Remove Them");
  gtk_widget_set_visible (ctx->cleanup_box, TRUE);
  ctx->measuring = g_cancellable_new ();
  proton_fetch_measure_async ((const char *const *) ctx->unused, ctx->measuring,
                              on_unused_measured, ctx);
}

static void
on_cleanup_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  FetchCtx *ctx = user_data;
  GError *error = NULL;
  if (proton_fetch_remove_finish (res, &error))
    {
      gtk_label_set_text (ctx->cleanup_label, "Older versions removed.");
      append_log_line (ctx, "Removed older GE-Proton versions no game used.");
    }
  else
    {
      gchar *msg = g_strdup_printf ("Could not remove everything: %s", error->message);
      gtk_label_set_text (ctx->cleanup_label, msg);
      append_log_line (ctx, msg);
      g_free (msg);
      g_clear_error (&error);
    }
  gtk_widget_set_visible (GTK_WIDGET (ctx->cleanup_button), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), TRUE);
  gog_dialog_window_set_can_close (ctx->window, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->action_button));
}

static void
on_cleanup_clicked (GtkButton *button, gpointer user_data)
{
  FetchCtx *ctx = user_data;
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), FALSE);
  /* Removing takes a moment; keep ctx alive until it is done. */
  gog_dialog_window_set_can_close (ctx->window, FALSE);
  gtk_label_set_text (ctx->cleanup_label, "Removing older versions…");
  /* A size measurement still running would land after this and put back
   * the "take up X" text for versions that are gone. */
  if (ctx->measuring)
    g_cancellable_cancel (ctx->measuring);
  proton_fetch_remove_async ((const char *const *) ctx->unused, NULL, on_cleanup_finished, ctx);
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
      offer_cleanup (ctx);
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Failed");
      append_log_line (ctx, result ? result : "Download failed.");
    }

  gtk_button_set_label (ctx->action_button, "Close");
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->action_button), TRUE);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), FALSE);
  gog_dialog_window_set_can_close (ctx->window, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->action_button));
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
  /* Closing mid-download would orphan the job; use Cancel instead. */
  gog_dialog_window_set_can_close (ctx->window, FALSE);
  gtk_widget_grab_focus (GTK_WIDGET (ctx->cancel_button));

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
  gtk_window_close (ctx->window);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  FetchCtx *ctx = user_data;
  if (ctx->job)
    gog_proton_fetch_job_cancel (ctx->job);
  else
    gtk_window_close (ctx->window);
}

static void
on_window_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  FetchCtx *ctx = user_data;
  if (ctx->on_done)
    ctx->on_done (ctx->succeeded, ctx->result, ctx->user_data);
  ctx_free (ctx);
}

void
gog_show_proton_fetch_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                              GogProtonFetchDoneFunc on_done, gpointer user_data)
{
  FetchCtx *ctx = g_new0 (FetchCtx, 1);
  ctx->parent = parent;
  ctx->config = config;
  ctx->library = library;
  ctx->on_done = on_done;
  ctx->user_data = user_data;

  ctx->window = gog_dialog_window_new (parent, "Download GE-Proton", 520, -1, 360, 200);

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

  ctx->cleanup_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_visible (ctx->cleanup_box, FALSE);
  ctx->cleanup_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (ctx->cleanup_label, TRUE);
  gtk_label_set_xalign (ctx->cleanup_label, 0);
  gtk_widget_set_hexpand (GTK_WIDGET (ctx->cleanup_label), TRUE);
  gtk_box_append (GTK_BOX (ctx->cleanup_box), GTK_WIDGET (ctx->cleanup_label));
  ctx->cleanup_button = GTK_BUTTON (gtk_button_new_with_label ("Remove Them"));
  gtk_widget_set_valign (GTK_WIDGET (ctx->cleanup_button), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->cleanup_button), "destructive-action");
  g_signal_connect (ctx->cleanup_button, "clicked", G_CALLBACK (on_cleanup_clicked), ctx);
  gtk_box_append (GTK_BOX (ctx->cleanup_box), GTK_WIDGET (ctx->cleanup_button));
  gtk_box_append (GTK_BOX (outer), ctx->cleanup_box);

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
  adw_window_set_content (ADW_WINDOW (ctx->window), toolbar_view);

  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (ctx->window);
}
