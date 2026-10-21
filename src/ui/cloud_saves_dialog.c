/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "cloud_saves_dialog.h"
#include "dialog_window.h"
#include "login_dialog.h"
#include "../cloud_saves.h"
#include "../runner.h"

typedef enum
{
  ACTION_LIST,
  ACTION_SYNC,     /* fetch what is newer on GOG, send what is newer here */
  ACTION_DOWNLOAD, /* only fetch */
  ACTION_UPLOAD,   /* only send */
  ACTION_REPLACE,  /* fetch everything that differs, newer here or not */
} Action;

typedef struct
{
  GtkWindow *window;
  GogGame *game; /* reffed */
  GCancellable *cancellable; /* while a task runs */
  GPtrArray *files;          /* GogCloudFile*, from the last listing; NULL before */
  GtkLabel *status;
  GtkProgressBar *bar;
  GtkListBox *list;
  GtkWidget *sync;
  GtkWidget *download;
  GtkWidget *upload;
  GtkWidget *replace;
  GtkWidget *login;
  GtkWidget *cancel;
  GtkWidget *close;
  guint pulse_id;
} CloudCtx;

/* What the worker needs, and what it brings back. */
typedef struct
{
  Action action;
  gint64 product_id;
  gchar *install_dir;
  gchar *wine_root;
  gchar *wine_user;
  GWeakRef window; /* for progress */
  /* results */
  GPtrArray *files;
  guint downloaded;
  guint uploaded;
  gchar *backup_dir; /* NULL unless a file here was replaced */
} Task;

static void
task_free (Task *t)
{
  g_free (t->install_dir);
  g_free (t->wine_root);
  g_free (t->wine_user);
  g_weak_ref_clear (&t->window);
  if (t->files)
    g_ptr_array_unref (t->files);
  g_free (t->backup_dir);
  g_free (t);
}

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
ctx_free (CloudCtx *ctx)
{
  stop_pulse (ctx);
  if (ctx->cancellable)
    g_cancellable_cancel (ctx->cancellable);
  g_clear_object (&ctx->cancellable);
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

static guint
count_state (GPtrArray *files, GogCloudState state)
{
  guint n = 0;
  for (guint i = 0; files && i < files->len; i++)
    if (((GogCloudFile *) g_ptr_array_index (files, i))->state == state)
      n++;
  return n;
}

/* ---- the worker ---- */

typedef struct
{
  GWeakRef window;
  gchar *text;
} Progress;

static gboolean
show_progress (gpointer data)
{
  Progress *p = data;
  GtkWindow *window = g_weak_ref_get (&p->window);
  CloudCtx *ctx = window ? g_object_get_data (G_OBJECT (window), "cloud-saves") : NULL;
  if (ctx && ctx->cancellable)
    gtk_label_set_text (ctx->status, p->text);
  if (window)
    g_object_unref (window);
  g_weak_ref_clear (&p->window);
  g_free (p->text);
  g_free (p);
  return G_SOURCE_REMOVE;
}

/* Shows text in the window, from the worker. Takes text. */
static void
post_progress (Task *t, gchar *text)
{
  Progress *p = g_new0 (Progress, 1);
  GtkWindow *window = g_weak_ref_get (&t->window);
  g_weak_ref_init (&p->window, window);
  if (window)
    g_object_unref (window);
  p->text = text;
  g_idle_add (show_progress, p);
}

typedef struct
{
  Task *task;
  const char *verb;
} FileProgress;

static void
on_file (guint done, guint total, const char *name, gpointer user_data)
{
  FileProgress *fp = user_data;
  post_progress (fp->task, g_strdup_printf ("%s %s (%u of %u)…", fp->verb, name, done + 1, total));
}

/* The files of listing in the given states. Transfer container. */
static GPtrArray *
pick (GPtrArray *listing, const GogCloudState *states, guint n_states)
{
  GPtrArray *out = g_ptr_array_new ();
  for (guint i = 0; i < listing->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (listing, i);
      for (guint s = 0; s < n_states; s++)
        if (file->state == states[s])
          g_ptr_array_add (out, file);
    }
  return out;
}

static void
run_task (GTask *gtask, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void) source;
  Task *t = data;
  GError *error = NULL;
  GogCloudGame game = { t->product_id, t->install_dir, t->wine_root, t->wine_user };

  post_progress (t, g_strdup ("Asking GOG where the game keeps its saves…"));
  GogCloudSession *session = gog_cloud_connect (&game, cancellable, &error);
  GPtrArray *listing = NULL;
  if (session)
    {
      post_progress (t, g_strdup ("Listing the saves…"));
      listing = gog_cloud_list (session, cancellable, &error);
    }

  gboolean ok = listing != NULL;
  if (ok && t->action != ACTION_LIST)
    {
      static const GogCloudState fetch_newer[] = { GOG_CLOUD_ONLY_ON_GOG, GOG_CLOUD_NEWER_ON_GOG };
      static const GogCloudState fetch_all[] = { GOG_CLOUD_ONLY_ON_GOG, GOG_CLOUD_NEWER_ON_GOG,
                                                 GOG_CLOUD_NEWER_HERE };
      static const GogCloudState send[] = { GOG_CLOUD_ONLY_HERE, GOG_CLOUD_NEWER_HERE };
      gboolean fetch = t->action != ACTION_UPLOAD;
      gboolean put = t->action == ACTION_SYNC || t->action == ACTION_UPLOAD;
      GPtrArray *down = !fetch                       ? g_ptr_array_new ()
                        : t->action == ACTION_REPLACE ? pick (listing, fetch_all, 3)
                                                      : pick (listing, fetch_newer, 2);
      GPtrArray *up = put ? pick (listing, send, 2) : g_ptr_array_new ();

      gboolean replaces = FALSE;
      for (guint i = 0; i < down->len; i++)
        replaces = replaces || ((GogCloudFile *) g_ptr_array_index (down, i))->local_mtime > 0;
      gchar *backup = replaces ? gog_cloud_backup_dir (t->product_id) : NULL;
      FileProgress fp = { t, "Downloading" };
      ok = gog_cloud_download (session, down, backup, on_file, &fp, cancellable, &error);
      if (ok)
        {
          t->downloaded = down->len;
          t->backup_dir = g_steal_pointer (&backup);
          fp.verb = "Uploading";
          ok = gog_cloud_upload (session, up, on_file, &fp, cancellable, &error);
          if (ok)
            t->uploaded = up->len;
        }
      g_free (backup);
      g_ptr_array_unref (down);
      g_ptr_array_unref (up);
      g_ptr_array_unref (listing);
      listing = NULL;
      if (ok)
        {
          post_progress (t, g_strdup ("Listing the saves…"));
          listing = gog_cloud_list (session, cancellable, &error);
          ok = listing != NULL;
        }
    }
  gog_cloud_session_free (session);
  t->files = listing;
  if (ok)
    g_task_return_boolean (gtask, TRUE);
  else
    g_task_return_error (gtask, error);
}

/* ---- the window ---- */

static const char *
state_text (GogCloudState state)
{
  switch (state)
    {
    case GOG_CLOUD_IN_SYNC: return "The same here and on GOG";
    case GOG_CLOUD_ONLY_ON_GOG: return "Only on GOG";
    case GOG_CLOUD_ONLY_HERE: return "Only here, not on GOG yet";
    case GOG_CLOUD_NEWER_ON_GOG: return "Newer on GOG";
    case GOG_CLOUD_NEWER_HERE: return "Newer here";
    default: return "";
    }
}

static gchar *
format_time (gint64 t)
{
  GDateTime *dt = g_date_time_new_from_unix_local (t);
  gchar *text = g_date_time_format (dt, "%-d %B %Y, %H:%M");
  g_date_time_unref (dt);
  return text;
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
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), file->name);
      gchar *subtitle;
      if (file->state == GOG_CLOUD_NEWER_ON_GOG || file->state == GOG_CLOUD_NEWER_HERE)
        {
          gchar *here = format_time (file->local_mtime);
          gchar *gog = format_time (file->cloud_mtime);
          subtitle = g_strdup_printf ("%s (here %s, on GOG %s)", state_text (file->state), here, gog);
          g_free (gog);
          g_free (here);
        }
      else
        subtitle = g_strdup (state_text (file->state));
      adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
      g_free (subtitle);
      gtk_list_box_append (ctx->list, row);
    }
}

/* "1 file", "2 files". Transfer full. */
static gchar *
count_text (guint n, const char *one, const char *several)
{
  return g_strdup_printf ("%u %s", n, n == 1 ? one : several);
}

/* What was done, and how things stand. Transfer full. */
static gchar *
describe (CloudCtx *ctx, Task *t)
{
  GString *out = g_string_new (NULL);
  if (t->action != ACTION_LIST)
    {
      if (t->downloaded == 0 && t->uploaded == 0)
        g_string_append (out, "Nothing needed moving. ");
      if (t->downloaded > 0)
        {
          gchar *n = count_text (t->downloaded, "file", "files");
          g_string_append_printf (out, "Downloaded %s. ", n);
          g_free (n);
        }
      if (t->uploaded > 0)
        {
          gchar *n = count_text (t->uploaded, "file", "files");
          g_string_append_printf (out, "Uploaded %s. ", n);
          g_free (n);
        }
      if (t->backup_dir)
        g_string_append_printf (out, "The saves replaced here were copied to %s first. ",
                                t->backup_dir);
    }

  GPtrArray *files = ctx->files;
  if (files->len == 0)
    {
      g_string_append (out, "No saves yet, here or on GOG.");
      return g_string_free (out, FALSE);
    }
  guint same = count_state (files, GOG_CLOUD_IN_SYNC);
  gchar *n = count_text (files->len, "save file", "save files");
  if (same == files->len)
    g_string_append_printf (out, "%s, all the same here and on GOG.", n);
  else
    {
      g_string_append_printf (out, "%s:", n);
      static const struct
      {
        GogCloudState state;
        const char *text;
      } parts[] = {
        { GOG_CLOUD_NEWER_ON_GOG, "newer on GOG" },
        { GOG_CLOUD_ONLY_ON_GOG, "only on GOG" },
        { GOG_CLOUD_NEWER_HERE, "newer here" },
        { GOG_CLOUD_ONLY_HERE, "only here" },
        { GOG_CLOUD_IN_SYNC, "the same" },
      };
      gboolean first = TRUE;
      for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
        {
          guint k = count_state (files, parts[i].state);
          if (k == 0)
            continue;
          g_string_append_printf (out, "%s %u %s", first ? "" : ",", k, parts[i].text);
          first = FALSE;
        }
      g_string_append_c (out, '.');
    }
  g_free (n);
  return g_string_free (out, FALSE);
}

static void
set_running (CloudCtx *ctx, gboolean running)
{
  gtk_widget_set_visible (ctx->cancel, running);
  gtk_widget_set_sensitive (ctx->cancel, TRUE);
  gtk_widget_set_visible (ctx->close, !running);
  gtk_widget_set_visible (GTK_WIDGET (ctx->bar), running);
  if (running)
    gtk_widget_set_visible (ctx->login, FALSE);
  GPtrArray *f = running ? NULL : ctx->files;
  guint to_fetch = count_state (f, GOG_CLOUD_ONLY_ON_GOG) + count_state (f, GOG_CLOUD_NEWER_ON_GOG);
  guint to_send = count_state (f, GOG_CLOUD_ONLY_HERE) + count_state (f, GOG_CLOUD_NEWER_HERE);
  gtk_widget_set_sensitive (ctx->sync, to_fetch + to_send > 0);
  gtk_widget_set_sensitive (ctx->download, to_fetch > 0);
  gtk_widget_set_sensitive (ctx->upload, to_send > 0);
  gtk_widget_set_visible (ctx->replace, count_state (f, GOG_CLOUD_NEWER_HERE) > 0);
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
on_task_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GWeakRef *ref = user_data;
  GtkWindow *window = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);
  CloudCtx *ctx = window ? g_object_get_data (G_OBJECT (window), "cloud-saves") : NULL;
  if (!ctx)
    {
      if (window)
        g_object_unref (window);
      return;
    }
  Task *t = g_task_get_task_data (G_TASK (res));
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (res), &error);
  g_clear_object (&ctx->cancellable);

  gchar *text;
  gboolean needs_login = FALSE;
  if (ok)
    {
      if (ctx->files)
        g_ptr_array_unref (ctx->files);
      ctx->files = g_steal_pointer (&t->files);
      text = describe (ctx, t);
    }
  else
    {
      needs_login = g_error_matches (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NEEDS_LOGIN);
      text = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
                 ? g_strdup ("Stopped. Files already moved stay moved.")
                 : g_strdup (error->message);
      /* What was listed before may be out of date now. */
      if (t->action != ACTION_LIST)
        g_clear_pointer (&ctx->files, g_ptr_array_unref);
    }
  g_clear_error (&error);
  fill_list (ctx);
  gtk_label_set_text (ctx->status, text);
  gog_announce (GTK_WIDGET (ctx->window), text);
  g_free (text);
  set_running (ctx, FALSE);
  gtk_widget_set_visible (ctx->login, needs_login);
  gtk_widget_grab_focus (needs_login                           ? ctx->login
                         : gtk_widget_get_sensitive (ctx->sync) ? ctx->sync
                                                                : ctx->close);
  g_object_unref (window);
}

static void
start_task (CloudCtx *ctx, Action action)
{
  GogRunnerKind kind = gog_game_get_runner_kind (ctx->game);
  const char *install_dir = gog_game_get_install_path (ctx->game);
  Task *t = g_new0 (Task, 1);
  t->action = action;
  t->product_id = gog_game_get_id (ctx->game);
  t->install_dir = g_strdup (install_dir);
  t->wine_root = runner_wine_root_dir (kind, install_dir);
  t->wine_user = g_strdup (kind == GOG_RUNNER_PROTON ? "steamuser" : g_get_user_name ());
  g_weak_ref_init (&t->window, ctx->window);

  ctx->cancellable = g_cancellable_new ();
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, ctx->window);
  GTask *task = g_task_new (NULL, ctx->cancellable, on_task_done, ref);
  g_task_set_task_data (task, t, (GDestroyNotify) task_free);
  gtk_label_set_text (ctx->status, "Starting…");
  set_running (ctx, TRUE);
  g_task_run_in_thread (task, run_task);
  g_object_unref (task);
  gtk_widget_grab_focus (ctx->cancel);
}

static void
on_sync_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  start_task (user_data, ACTION_SYNC);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  start_task (user_data, ACTION_DOWNLOAD);
}

static void
on_upload_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  start_task (user_data, ACTION_UPLOAD);
}

static void
on_replace_response (const char *response, gpointer user_data)
{
  if (g_strcmp0 (response, "replace") == 0)
    start_task (user_data, ACTION_REPLACE);
}

static void
on_replace_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  CloudCtx *ctx = user_data;
  guint n = count_state (ctx->files, GOG_CLOUD_NEWER_HERE);
  gchar *what = count_text (n, "save file is", "save files are");
  gchar *body = g_strdup_printf ("%s newer here than on GOG. Downloading GOG's copies replaces "
                                 "%s, and the progress made since is lost here. The files "
                                 "replaced are copied to a backup folder first. Saves only "
                                 "here are left alone.",
                                 what, n == 1 ? "it" : "them");
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
  if (ctx && logged_in && !ctx->cancellable)
    start_task (ctx, ACTION_LIST);
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
  if (ctx->cancellable)
    g_cancellable_cancel (ctx->cancellable);
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
  gtk_label_set_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
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
  ctx->window = gog_dialog_window_new (parent, heading, 680, 560, 360, 360);
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
                                 "and GOG Galaxy on Windows can use them. Sync downloads the "
                                 "saves that are newer on GOG and uploads the ones that are "
                                 "newer here; it never replaces a newer save. GOG Manager uses "
                                 "lgogdownloader's GOG login without changing it.",
                                 TRUE));

  ctx->status = GTK_LABEL (wrapped_label ("Starting…", FALSE));
  gtk_label_set_selectable (ctx->status, TRUE);
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
                                  "Save files", -1);
  gtk_widget_set_visible (GTK_WIDGET (ctx->list), FALSE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), GTK_WIDGET (ctx->list));
  gtk_box_append (GTK_BOX (box), scroll);

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ctx->login = gtk_button_new_with_mnemonic ("_Log In to GOG…");
  gtk_widget_add_css_class (ctx->login, "suggested-action");
  gtk_widget_set_visible (ctx->login, FALSE);
  g_signal_connect (ctx->login, "clicked", G_CALLBACK (on_login_clicked), ctx);
  ctx->replace = gtk_button_new_with_mnemonic ("_Replace with GOG's…");
  gtk_widget_add_css_class (ctx->replace, "destructive-action");
  gtk_widget_set_tooltip_text (ctx->replace, "Download GOG's copies of the saves that are newer "
                                             "here too, after copying them to a backup folder");
  g_signal_connect (ctx->replace, "clicked", G_CALLBACK (on_replace_clicked), ctx);
  ctx->upload = gtk_button_new_with_mnemonic ("_Upload");
  gtk_widget_set_tooltip_text (ctx->upload, "Only upload the saves that are newer here or only here");
  g_signal_connect (ctx->upload, "clicked", G_CALLBACK (on_upload_clicked), ctx);
  ctx->download = gtk_button_new_with_mnemonic ("_Download");
  gtk_widget_set_tooltip_text (ctx->download, "Only download the saves that are newer on GOG or "
                                              "only there");
  g_signal_connect (ctx->download, "clicked", G_CALLBACK (on_download_clicked), ctx);
  ctx->sync = gtk_button_new_with_mnemonic ("_Sync");
  gtk_widget_add_css_class (ctx->sync, "suggested-action");
  gtk_widget_set_tooltip_text (ctx->sync, "Download what is newer on GOG and upload what is "
                                          "newer here");
  g_signal_connect (ctx->sync, "clicked", G_CALLBACK (on_sync_clicked), ctx);
  ctx->cancel = gtk_button_new_with_mnemonic ("_Cancel");
  g_signal_connect (ctx->cancel, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->close = gtk_button_new_with_mnemonic ("C_lose");
  g_signal_connect (ctx->close, "clicked", G_CALLBACK (on_close_clicked), ctx);
  GtkWidget *order[] = { ctx->login, ctx->replace, ctx->upload, ctx->download, ctx->sync,
                         ctx->cancel, ctx->close };
  for (guint i = 0; i < G_N_ELEMENTS (order); i++)
    gtk_box_append (GTK_BOX (buttons), order[i]);
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_window_set_content (ADW_WINDOW (ctx->window), view);

  start_task (ctx, ACTION_LIST);
  gtk_window_present (ctx->window);
}
