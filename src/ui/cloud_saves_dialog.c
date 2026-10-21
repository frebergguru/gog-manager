/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "cloud_saves_dialog.h"
#include "dialog_window.h"
#include "login_dialog.h"
#include "../gog_sync.h"
#include "../runner.h"

typedef struct
{
  GtkWindow *window;
  GogGame *game; /* reffed */
  GogSyncJob *job;
  GogCloudAction action;  /* of job */
  guint n_requested;      /* files asked for by a download */
  GPtrArray *files;       /* GogCloudFile*, from the last listing; NULL before */
  GtkLabel *status;
  GtkProgressBar *bar;
  GtkListBox *list;
  GtkTextView *log;
  GtkWidget *download;
  GtkWidget *upload;
  GtkWidget *replace;
  GtkWidget *login; /* shown when lgogdownloader has to log in again */
  GtkWidget *cancel;
  GtkWidget *close;
  guint pulse_id;
} CloudCtx;

gboolean
gog_cloud_saves_available (GogGame *game)
{
  GogRunnerKind kind = gog_game_get_runner_kind (game);
  return gog_game_get_installed (game) && !gog_game_get_base_game (game) &&
         gog_game_get_id (game) > 0 && gog_game_get_install_path (game) &&
         (kind == GOG_RUNNER_WINE || kind == GOG_RUNNER_PROTON);
}

static void
stop_pulse (CloudCtx *ctx)
{
  if (ctx->pulse_id)
    g_source_remove (ctx->pulse_id);
  ctx->pulse_id = 0;
}

static void
drop_job (CloudCtx *ctx)
{
  if (!ctx->job)
    return;
  /* Its queued signals may still come after the window is gone. */
  g_signal_handlers_disconnect_by_data (ctx->job, ctx);
  gog_sync_job_cancel (ctx->job);
  g_clear_object (&ctx->job);
}

static void
ctx_free (CloudCtx *ctx)
{
  stop_pulse (ctx);
  drop_job (ctx);
  if (ctx->files)
    g_ptr_array_unref (ctx->files);
  g_object_unref (ctx->game);
  g_free (ctx);
}

static gboolean
pulse (gpointer data)
{
  gtk_progress_bar_pulse (((CloudCtx *) data)->bar);
  return G_SOURCE_CONTINUE;
}

static const char *
state_text (GogCloudFileState state)
{
  switch (state)
    {
    case GOG_CLOUD_FILE_ONLY_ON_GOG: return "Only on GOG";
    case GOG_CLOUD_FILE_NOT_NEWER: return "Here too, not newer than GOG's";
    case GOG_CLOUD_FILE_NEWER_HERE: return "Newer here than on GOG";
    case GOG_CLOUD_FILE_LARGER_HERE: return "Different here: larger than GOG's";
    default: return "";
    }
}

/* "1 save", "2 saves". Transfer full. */
static gchar *
count_text (guint n, const char *one, const char *several)
{
  return g_strdup_printf ("%u %s", n, n == 1 ? one : several);
}

static guint
count_state (GPtrArray *files, GogCloudFileState state)
{
  guint n = 0;
  for (guint i = 0; files && i < files->len; i++)
    if (((GogCloudFile *) g_ptr_array_index (files, i))->state == state)
      n++;
  return n;
}

/* Files on GOG that are the same, or newer, there: the ones a download takes.
 * Transfer container. */
static GPtrArray *
paths_to_fetch (CloudCtx *ctx, gboolean all)
{
  GPtrArray *paths = g_ptr_array_new ();
  for (guint i = 0; ctx->files && i < ctx->files->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (ctx->files, i);
      if (all || file->state == GOG_CLOUD_FILE_ONLY_ON_GOG || file->state == GOG_CLOUD_FILE_NOT_NEWER)
        g_ptr_array_add (paths, file->path);
    }
  g_ptr_array_add (paths, NULL);
  return paths;
}

static void
fill_list (CloudCtx *ctx)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (ctx->list))))
    gtk_list_box_remove (ctx->list, child);
  gboolean any = ctx->files && ctx->files->len > 0;
  gtk_widget_set_visible (GTK_WIDGET (ctx->list), any);
  for (guint i = 0; any && i < ctx->files->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (ctx->files, i);
      GtkWidget *row = adw_action_row_new ();
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->path);
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), state_text (file->state));
      gtk_list_box_append (ctx->list, row);
    }
}

/* What the listing says, and what the job before it did. Transfer full. */
static gchar *
describe (CloudCtx *ctx, GPtrArray *before)
{
  GString *out = g_string_new (NULL);
  if (ctx->action == GOG_CLOUD_DOWNLOAD)
    {
      gchar *n = count_text (ctx->n_requested, "file was", "files were");
      g_string_append_printf (out, "%s downloaded from GOG. ", n);
      g_free (n);
    }
  else if (ctx->action == GOG_CLOUD_UPLOAD)
    {
      /* Sent: what GOG has now that it had older, or didn't have. */
      guint sent = 0;
      for (guint i = 0; i < ctx->files->len; i++)
        {
          GogCloudFile *file = g_ptr_array_index (ctx->files, i);
          if (file->state != GOG_CLOUD_FILE_NOT_NEWER)
            continue;
          gboolean was_same = FALSE;
          for (guint j = 0; before && j < before->len; j++)
            {
              GogCloudFile *old = g_ptr_array_index (before, j);
              if (g_str_equal (old->path, file->path))
                was_same = old->state == GOG_CLOUD_FILE_NOT_NEWER;
            }
          if (!was_same)
            sent++;
        }
      if (sent == 0)
        g_string_append (out, "No save here was newer than GOG's, so nothing was uploaded. ");
      else
        {
          gchar *n = count_text (sent, "file was", "files were");
          g_string_append_printf (out, "%s uploaded to GOG. ", n);
          g_free (n);
        }
    }

  guint total = ctx->files->len;
  if (total == 0)
    {
      g_string_append (out, "GOG has no saves of this game yet.");
      return g_string_free (out, FALSE);
    }
  gchar *n = count_text (total, "file", "files");
  g_string_append_printf (out, "GOG has %s of this game's saves", n);
  g_free (n);
  guint only = count_state (ctx->files, GOG_CLOUD_FILE_ONLY_ON_GOG);
  guint newer = count_state (ctx->files, GOG_CLOUD_FILE_NEWER_HERE) +
                count_state (ctx->files, GOG_CLOUD_FILE_LARGER_HERE);
  if (only == 0 && newer == 0)
    g_string_append (out, ", and every one is here too.");
  else
    {
      g_string_append (out, ":");
      if (only > 0)
        {
          n = count_text (only, "is", "are");
          g_string_append_printf (out, " %s only on GOG%s", n, newer > 0 ? ";" : ".");
          g_free (n);
        }
      if (newer > 0)
        {
          n = count_text (newer, "is", "are");
          g_string_append_printf (out, " %s different here, and kept as it is unless you "
                                       "replace %s.",
                                  n, newer == 1 ? "it" : "them");
          g_free (n);
        }
    }
  return g_string_free (out, FALSE);
}

static void
set_running (CloudCtx *ctx, gboolean running)
{
  gtk_widget_set_visible (ctx->cancel, running);
  gtk_widget_set_sensitive (ctx->cancel, TRUE);
  gtk_widget_set_visible (ctx->close, !running);
  if (running)
    gtk_widget_set_visible (ctx->login, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (ctx->bar), running);
  gboolean listed = !running && ctx->files;
  gtk_widget_set_sensitive (ctx->download, listed && count_state (ctx->files, GOG_CLOUD_FILE_ONLY_ON_GOG) +
                                                         count_state (ctx->files, GOG_CLOUD_FILE_NOT_NEWER) > 0);
  gtk_widget_set_sensitive (ctx->upload, listed);
  gtk_widget_set_visible (ctx->replace, listed &&
                                          count_state (ctx->files, GOG_CLOUD_FILE_NEWER_HERE) +
                                          count_state (ctx->files, GOG_CLOUD_FILE_LARGER_HERE) > 0);
  gog_dialog_window_set_can_close (ctx->window, !running);
  stop_pulse (ctx);
  if (running)
    {
      /* Reduced motion: a still bar, the text says what's going on. */
      gboolean animate = TRUE;
      g_object_get (gtk_settings_get_default (), "gtk-enable-animations", &animate, NULL);
      if (animate)
        ctx->pulse_id = g_timeout_add (150, pulse, ctx);
    }
}

static void
on_progress (GogSyncJob *job, gdouble fraction, const gchar *message, gpointer user_data)
{
  (void) job;
  (void) fraction;
  gtk_label_set_text (((CloudCtx *) user_data)->status, message);
}

static void
on_log_line (GogSyncJob *job, const gchar *line, gpointer user_data)
{
  (void) job;
  CloudCtx *ctx = user_data;
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (ctx->log);
  GtkTextIter end;
  gtk_text_buffer_get_end_iter (buffer, &end);
  gtk_text_buffer_insert (buffer, &end, line, -1);
  gtk_text_buffer_insert (buffer, &end, "\n", 1);
}

static void
on_finished (GogSyncJob *job, gboolean success, const gchar *error_message, gpointer user_data)
{
  CloudCtx *ctx = user_data;
  gchar *text;
  GPtrArray *before = ctx->files;
  ctx->files = NULL;
  if (success)
    {
      ctx->files = g_ptr_array_ref (gog_sync_job_get_cloud_files (job));
      text = describe (ctx, before);
    }
  else
    text = g_strdup_printf ("%s%s", ctx->action == GOG_CLOUD_SHOW ? "" : "It stopped: ",
                            error_message ? error_message : "lgogdownloader failed");
  if (before)
    g_ptr_array_unref (before);
  gboolean needs_login = !success && gog_sync_job_needs_login (job);
  drop_job (ctx);
  fill_list (ctx);
  gtk_label_set_text (ctx->status, text);
  gog_announce (GTK_WIDGET (ctx->window), text);
  g_free (text);
  set_running (ctx, FALSE);
  gtk_widget_set_visible (ctx->login, needs_login);
  gtk_widget_grab_focus (needs_login                               ? ctx->login
                         : gtk_widget_get_sensitive (ctx->download) ? ctx->download
                                                                    : ctx->close);
}

static void
start_job (CloudCtx *ctx, GogCloudAction action, const char *const *paths)
{
  const char *install_dir = gog_game_get_install_path (ctx->game);
  GogRunnerKind kind = gog_game_get_runner_kind (ctx->game);
  gchar *root = runner_wine_root_dir (kind, install_dir);
  ctx->action = action;
  ctx->n_requested = paths ? g_strv_length ((gchar **) paths) : 0;
  ctx->job = gog_sync_job_new_cloud (action, gog_game_get_id (ctx->game), install_dir, root,
                                     gog_sync_cloud_wine_user (kind == GOG_RUNNER_PROTON), paths);
  g_free (root);
  g_signal_connect (ctx->job, "progress", G_CALLBACK (on_progress), ctx);
  g_signal_connect (ctx->job, "log-line", G_CALLBACK (on_log_line), ctx);
  g_signal_connect (ctx->job, "finished", G_CALLBACK (on_finished), ctx);
  gtk_label_set_text (ctx->status, "Starting…");
  set_running (ctx, TRUE);
  gog_sync_job_start (ctx->job);
  gtk_widget_grab_focus (ctx->cancel);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  CloudCtx *ctx = user_data;
  GPtrArray *paths = paths_to_fetch (ctx, FALSE);
  start_job (ctx, GOG_CLOUD_DOWNLOAD, (const char *const *) paths->pdata);
  g_ptr_array_unref (paths);
}

static void
on_upload_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  start_job (user_data, GOG_CLOUD_UPLOAD, NULL);
}

static void
on_replace_response (const char *response, gpointer user_data)
{
  CloudCtx *ctx = user_data;
  if (g_strcmp0 (response, "replace") != 0)
    return;
  GPtrArray *paths = paths_to_fetch (ctx, TRUE);
  start_job (ctx, GOG_CLOUD_DOWNLOAD, (const char *const *) paths->pdata);
  g_ptr_array_unref (paths);
}

static void
on_replace_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  CloudCtx *ctx = user_data;
  guint n = count_state (ctx->files, GOG_CLOUD_FILE_NEWER_HERE) +
            count_state (ctx->files, GOG_CLOUD_FILE_LARGER_HERE);
  gchar *what = count_text (n, "save file here differs", "save files here differ");
  gchar *body = g_strdup_printf ("%s from GOG's copy. Downloading GOG's copies overwrites "
                                 "%s, and the progress in %s is lost. Saves only here are "
                                 "left alone.",
                                 what, n == 1 ? "it" : "them", n == 1 ? "it" : "them");
  GogAlert *alert = gog_alert_new ("Replace Saves Here with GOG's?", body);
  gog_alert_add_response (alert, "cancel", "_Cancel", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "replace", "_Replace", ADW_RESPONSE_DESTRUCTIVE);
  gog_alert_set_default_response (alert, "cancel");
  gog_alert_set_close_response (alert, "cancel");
  gog_alert_present (alert, ctx->window, on_replace_response, ctx);
  g_free (body);
  g_free (what);
}

/* The login window's callback comes from its "destroy", so this window
 * gets a weak ref. */
static void
on_logged_in (gboolean logged_in, gpointer user_data)
{
  GWeakRef *ref = user_data;
  GtkWindow *window = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);
  CloudCtx *ctx = window ? g_object_get_data (G_OBJECT (window), "cloud-saves") : NULL;
  if (ctx && logged_in && !ctx->job)
    start_job (ctx, GOG_CLOUD_SHOW, NULL);
  if (window)
    g_object_unref (window);
}

static void
on_login_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  CloudCtx *ctx = user_data;
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, ctx->window);
  gog_show_login_dialog (ctx->window, on_logged_in, ref);
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  CloudCtx *ctx = user_data;
  gtk_widget_set_sensitive (GTK_WIDGET (button), FALSE);
  gtk_label_set_text (ctx->status, "Stopping…");
  if (ctx->job)
    gog_sync_job_cancel (ctx->job);
}

static void
on_close_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  gtk_window_close (((CloudCtx *) user_data)->window);
}

static GtkWidget *
wrapped_label (const char *text, gboolean dim)
{
  GtkWidget *label = gtk_label_new (text);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  if (dim)
    gtk_widget_add_css_class (label, "dim-label");
  return label;
}

void
gog_show_cloud_saves_dialog (GtkWindow *parent, GogGame *game)
{
  CloudCtx *ctx = g_new0 (CloudCtx, 1);
  ctx->game = g_object_ref (game);
  gchar *heading = g_strdup_printf ("Cloud Saves: %s", gog_game_get_title (game));
  ctx->window = gog_dialog_window_new (parent, heading, 640, 560, 360, 360);
  g_free (heading);
  g_object_set_data_full (G_OBJECT (ctx->window), "cloud-saves", ctx, (GDestroyNotify) ctx_free);

  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  gtk_box_append (GTK_BOX (box),
                  wrapped_label ("GOG keeps this game's saves for GOG Galaxy, so other computers "
                                 "and GOG Galaxy on Windows can use them. lgogdownloader moves "
                                 "them between GOG and the game's Wine prefix. Download takes "
                                 "what is only on GOG and what isn't newer here; Upload sends "
                                 "the saves that are newer here or only here. Neither replaces "
                                 "a newer save.",
                                 TRUE));

  ctx->status = GTK_LABEL (wrapped_label ("Starting…", FALSE));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->status));

  ctx->bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->bar), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Working", -1);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->bar));

  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (scroll, TRUE);
  ctx->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (ctx->list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->list), "boxed-list");
  gtk_widget_set_valign (GTK_WIDGET (ctx->list), GTK_ALIGN_START);
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Saves on GOG", -1);
  gtk_widget_set_visible (GTK_WIDGET (ctx->list), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (ctx->list));
  gtk_box_append (GTK_BOX (box), scroll);

  GtkWidget *expander = gtk_expander_new_with_mnemonic ("lgogdownloader _output");
  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (log_scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (log_scroll), 120);
  gtk_widget_add_css_class (log_scroll, "card");
  ctx->log = GTK_TEXT_VIEW (gtk_text_view_new ());
  gtk_text_view_set_editable (ctx->log, FALSE);
  gtk_text_view_set_monospace (ctx->log, TRUE);
  gtk_text_view_set_wrap_mode (ctx->log, GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->log), "gog-log-view");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->log), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "lgogdownloader output", -1);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (log_scroll), GTK_WIDGET (ctx->log));
  gtk_expander_set_child (GTK_EXPANDER (expander), log_scroll);
  gtk_box_append (GTK_BOX (box), expander);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ctx->replace = gtk_button_new_with_mnemonic ("_Replace with GOG's…");
  gtk_widget_add_css_class (ctx->replace, "destructive-action");
  gtk_widget_set_tooltip_text (ctx->replace, "Download GOG's copies of the saves that differ "
                                             "here too, overwriting them");
  g_signal_connect (ctx->replace, "clicked", G_CALLBACK (on_replace_clicked), ctx);
  ctx->login = gtk_button_new_with_mnemonic ("_Log In to GOG…");
  gtk_widget_add_css_class (ctx->login, "suggested-action");
  gtk_widget_set_visible (ctx->login, FALSE);
  g_signal_connect (ctx->login, "clicked", G_CALLBACK (on_login_clicked), ctx);
  ctx->upload = gtk_button_new_with_mnemonic ("_Upload to GOG");
  g_signal_connect (ctx->upload, "clicked", G_CALLBACK (on_upload_clicked), ctx);
  ctx->download = gtk_button_new_with_mnemonic ("_Download from GOG");
  g_signal_connect (ctx->download, "clicked", G_CALLBACK (on_download_clicked), ctx);
  ctx->cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect (ctx->cancel, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->close = gtk_button_new_with_mnemonic ("C_lose");
  g_signal_connect (ctx->close, "clicked", G_CALLBACK (on_close_clicked), ctx);
  gtk_box_append (GTK_BOX (buttons), ctx->login);
  gtk_box_append (GTK_BOX (buttons), ctx->replace);
  gtk_box_append (GTK_BOX (buttons), ctx->upload);
  gtk_box_append (GTK_BOX (buttons), ctx->download);
  gtk_box_append (GTK_BOX (buttons), ctx->cancel);
  gtk_box_append (GTK_BOX (buttons), ctx->close);
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_window_set_content (ADW_WINDOW (ctx->window), view);

  if (!gog_sync_is_available ())
    {
      gtk_label_set_text (ctx->status, "This needs lgogdownloader, which was not found.");
      set_running (ctx, FALSE);
      gtk_window_present (ctx->window);
      gtk_widget_grab_focus (ctx->close);
      return;
    }
  start_job (ctx, GOG_CLOUD_SHOW, NULL);
  gtk_window_present (ctx->window);
}
