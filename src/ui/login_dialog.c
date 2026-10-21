/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "login_dialog.h"
#include "dialog_window.h"
#include "../gog_sync.h"
#include <string.h>

/* Where GOG sends the browser once the login is done; lgogdownloader reads
 * the code from it. */
#define SUCCESS_PREFIX "https://embed.gog.com/on_login_success"

typedef struct
{
  GtkWindow *window;
  GogLoginDoneFunc on_done;
  gpointer user_data;
  GSubprocess *proc;       /* lgogdownloader --browser-login */
  GDataInputStream *out;
  GCancellable *cancellable;
  gchar *login_url;
  gboolean logged_in;
  gboolean gone;           /* the window is closed; late callbacks do nothing */
  guint pending;           /* async calls not back yet; the last one frees ctx */

  GtkLabel *status;
  GtkWidget *open_button;
  GtkWidget *entry;
  GtkWidget *login_button;
  GtkWidget *close_button;
} LoginCtx;

static void
ctx_free (LoginCtx *ctx)
{
  if (ctx->pending > 0 || !ctx->gone)
    return;
  g_free (ctx->login_url);
  g_clear_object (&ctx->out);
  g_clear_object (&ctx->proc);
  g_clear_object (&ctx->cancellable);
  g_free (ctx);
}

/* Each async callback starts with this: FALSE when the window is gone,
 * after freeing ctx if nothing else is pending. */
static gboolean
returned (LoginCtx *ctx)
{
  ctx->pending--;
  if (!ctx->gone)
    return TRUE;
  ctx_free (ctx);
  return FALSE;
}

static void
set_status (LoginCtx *ctx, const char *text, gboolean announce)
{
  gtk_label_set_text (ctx->status, text);
  if (announce)
    gog_announce (GTK_WIDGET (ctx->window), text);
}

static void
finish (LoginCtx *ctx, const char *text)
{
  set_status (ctx, text, TRUE);
  gtk_widget_set_sensitive (ctx->open_button, FALSE);
  gtk_widget_set_sensitive (ctx->entry, FALSE);
  gtk_widget_set_sensitive (ctx->login_button, FALSE);
  gtk_button_set_label (GTK_BUTTON (ctx->close_button), "Close");
  gtk_widget_add_css_class (ctx->close_button, "suggested-action");
  gtk_widget_grab_focus (ctx->close_button);
}

/* ---- the login check afterwards ---- */

static void
on_status_checked (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  gchar *out = NULL;
  gboolean ran = g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL,
                                                       NULL);
  if (returned (ctx))
    {
      ctx->logged_in = ran && out && strstr (out, "Login status: Logged in");
      finish (ctx, ctx->logged_in
                       ? "You are logged in to GOG. GOG Manager can now check for new games "
                         "and updates, and download them."
                       : "The login did not work. Close this and try again; GOG's page "
                         "address must be copied after the login is complete.");
    }
  g_free (out);
  g_object_unref (source);
}

static void
check_status (LoginCtx *ctx)
{
  const char *args[] = { "--check-login-status", NULL };
  gchar **argv = gog_sync_lgog_argv (args);
  GSubprocess *proc = g_subprocess_newv ((const gchar *const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                             G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         NULL);
  g_free (argv);
  if (!proc)
    {
      finish (ctx, "Could not run lgogdownloader to check the login.");
      return;
    }
  ctx->pending++;
  g_subprocess_communicate_utf8_async (proc, NULL, ctx->cancellable, on_status_checked, ctx);
}

static void
on_login_exited (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  g_subprocess_wait_finish (G_SUBPROCESS (source), res, NULL);
  if (!returned (ctx))
    return;
  set_status (ctx, "Checking the login…", FALSE);
  check_status (ctx);
}

/* ---- lgogdownloader --browser-login ---- */

static void read_next_line (LoginCtx *ctx);

static void
on_line (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  gsize len = 0;
  gchar *line = g_data_input_stream_read_line_finish_utf8 (G_DATA_INPUT_STREAM (source), res,
                                                          &len, NULL);
  if (!returned (ctx) || !line)
    {
      g_free (line);
      return;
    }
  g_strstrip (line);
  if (!ctx->login_url && g_str_has_prefix (line, "https://auth.gog.com/"))
    {
      ctx->login_url = g_strdup (line);
      gtk_widget_set_sensitive (ctx->open_button, TRUE);
      set_status (ctx, "Ready. Open GOG's login page to begin.", FALSE);
      gtk_widget_grab_focus (ctx->open_button);
    }
  g_free (line);
  read_next_line (ctx);
}

static void
read_next_line (LoginCtx *ctx)
{
  ctx->pending++;
  g_data_input_stream_read_line_async (ctx->out, G_PRIORITY_DEFAULT, ctx->cancellable, on_line,
                                       ctx);
}

static void
start_login (LoginCtx *ctx)
{
  const char *args[] = { "--browser-login", NULL };
  gchar **argv = gog_sync_lgog_argv (args);
  GSubprocessLauncher *launcher = g_subprocess_launcher_new (
      G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
      G_SUBPROCESS_FLAGS_STDERR_MERGE);
  g_subprocess_launcher_setenv (launcher, "TERM", "dumb", TRUE);
  GError *error = NULL;
  ctx->proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, &error);
  g_object_unref (launcher);
  g_free (argv);
  if (!ctx->proc)
    {
      gchar *text = g_strdup_printf ("Could not run lgogdownloader: %s", error->message);
      finish (ctx, text);
      g_free (text);
      g_clear_error (&error);
      return;
    }
  ctx->out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (ctx->proc));
  read_next_line (ctx);
  ctx->pending++;
  g_subprocess_wait_async (ctx->proc, ctx->cancellable, on_login_exited, ctx);
}

/* ---- the buttons ---- */

static void
on_open_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  GError *error = NULL;
  gboolean launched = gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (source), res, &error);
  if (!returned (ctx))
    {
      g_clear_error (&error);
      return;
    }
  if (!launched && !g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    {
      gchar *text = g_strdup_printf ("Could not open the web browser (%s). Copy this address "
                                     "into it instead:\n%s",
                                     error->message, ctx->login_url);
      set_status (ctx, text, TRUE);
      gtk_label_set_selectable (ctx->status, TRUE);
      g_free (text);
    }
  g_clear_error (&error);
}

static void
on_open_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  LoginCtx *ctx = user_data;
  if (!ctx->login_url)
    return;
  GtkUriLauncher *launcher = gtk_uri_launcher_new (ctx->login_url);
  ctx->pending++;
  gtk_uri_launcher_launch (launcher, ctx->window, ctx->cancellable, on_open_finished, ctx);
  g_object_unref (launcher);
  set_status (ctx, "Log in on GOG's page in the browser. Then copy the address of the page "
                   "it ends on and paste it below.",
              TRUE);
  gtk_widget_grab_focus (ctx->entry);
}

static void
on_entry_changed (GtkEditable *entry, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  const char *text = gtk_editable_get_text (entry);
  gtk_widget_set_sensitive (ctx->login_button, ctx->proc && *text);
}

static void
on_login_clicked (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  LoginCtx *ctx = user_data;
  gchar *address = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (ctx->entry))));
  if (!g_str_has_prefix (address, SUCCESS_PREFIX) || !strstr (address, "code="))
    {
      set_status (ctx, "That isn't the address GOG ends the login on. It starts with "
                       SUCCESS_PREFIX " and has a code in it; copy it from the browser's "
                       "address bar once the login is done.",
                  TRUE);
      g_free (address);
      gtk_widget_grab_focus (ctx->entry);
      return;
    }
  /* The address goes to lgogdownloader, which turns its code into a login. */
  gchar *line = g_strconcat (address, "\n", NULL);
  GOutputStream *in = g_subprocess_get_stdin_pipe (ctx->proc);
  gboolean ok = g_output_stream_write_all (in, line, strlen (line), NULL, NULL, NULL) &&
                g_output_stream_close (in, NULL, NULL);
  g_free (line);
  g_free (address);
  gtk_widget_set_sensitive (ctx->login_button, FALSE);
  gtk_widget_set_sensitive (ctx->entry, FALSE);
  if (!ok)
    finish (ctx, "Could not pass the address on to lgogdownloader.");
  else
    set_status (ctx, "Logging in…", TRUE);
}

static void
on_close_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  gtk_window_close (((LoginCtx *) user_data)->window);
}

static void
on_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  LoginCtx *ctx = user_data;
  ctx->gone = TRUE;
  g_cancellable_cancel (ctx->cancellable);
  /* A login left half way must not linger, waiting for an address. */
  if (ctx->proc)
    g_subprocess_force_exit (ctx->proc);
  if (ctx->on_done)
    ctx->on_done (ctx->logged_in, ctx->user_data);
  ctx_free (ctx);
}

void
gog_show_login_dialog (GtkWindow *parent, GogLoginDoneFunc on_done, gpointer user_data)
{
  LoginCtx *ctx = g_new0 (LoginCtx, 1);
  ctx->on_done = on_done;
  ctx->user_data = user_data;
  ctx->cancellable = g_cancellable_new ();
  ctx->window = gog_dialog_window_new (parent, "Log In to GOG", 560, 440, 360, 320);
  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_destroy), ctx);

  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  GtkWidget *intro = gtk_label_new (
      "GOG Manager downloads your games with lgogdownloader, which needs to be logged in to "
      "your GOG account once. You log in on GOG's own page in your web browser; GOG Manager "
      "never sees your password.");
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_label_set_xalign (GTK_LABEL (intro), 0.0);
  gtk_box_append (GTK_BOX (box), intro);

  GtkWidget *step1 = gtk_label_new ("1. Open GOG's login page and log in there.");
  gtk_label_set_wrap (GTK_LABEL (step1), TRUE);
  gtk_label_set_xalign (GTK_LABEL (step1), 0.0);
  gtk_widget_add_css_class (step1, "heading");
  gtk_box_append (GTK_BOX (box), step1);
  ctx->open_button = gtk_button_new_with_label ("Open GOG Login Page");
  gtk_widget_add_css_class (ctx->open_button, "pill");
  gtk_widget_add_css_class (ctx->open_button, "suggested-action");
  gtk_widget_set_halign (ctx->open_button, GTK_ALIGN_START);
  gtk_widget_set_sensitive (ctx->open_button, FALSE);
  g_signal_connect (ctx->open_button, "clicked", G_CALLBACK (on_open_clicked), ctx);
  gtk_box_append (GTK_BOX (box), ctx->open_button);

  GtkWidget *step2 = gtk_label_new (
      "2. When GOG is done, the browser shows a page whose address starts with "
      "https://embed.gog.com/on_login_success (it may look empty). Copy that whole address "
      "from the address bar and paste it here:");
  gtk_label_set_wrap (GTK_LABEL (step2), TRUE);
  gtk_label_set_xalign (GTK_LABEL (step2), 0.0);
  gtk_widget_add_css_class (step2, "heading");
  gtk_box_append (GTK_BOX (box), step2);
  ctx->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->entry), SUCCESS_PREFIX "?…&code=…");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Address of the page GOG ends the login on", -1);
  g_signal_connect (ctx->entry, "changed", G_CALLBACK (on_entry_changed), ctx);
  g_signal_connect (ctx->entry, "activate", G_CALLBACK (on_login_clicked), ctx);
  gtk_box_append (GTK_BOX (box), ctx->entry);

  ctx->status = GTK_LABEL (gtk_label_new ("Starting lgogdownloader…"));
  gtk_label_set_wrap (ctx->status, TRUE);
  gtk_label_set_xalign (ctx->status, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->status), "dim-label");
  gtk_widget_set_vexpand (GTK_WIDGET (ctx->status), TRUE);
  gtk_widget_set_valign (GTK_WIDGET (ctx->status), GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->status));

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  ctx->close_button = gtk_button_new_with_label ("Cancel");
  g_signal_connect (ctx->close_button, "clicked", G_CALLBACK (on_close_clicked), ctx);
  ctx->login_button = gtk_button_new_with_label ("Log In");
  gtk_widget_add_css_class (ctx->login_button, "suggested-action");
  gtk_widget_set_sensitive (ctx->login_button, FALSE);
  g_signal_connect (ctx->login_button, "clicked", G_CALLBACK (on_login_clicked), ctx);
  gtk_box_append (GTK_BOX (buttons), ctx->close_button);
  gtk_box_append (GTK_BOX (buttons), ctx->login_button);
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_window_set_content (ADW_WINDOW (ctx->window), view);

  if (gog_sync_is_available ())
    start_login (ctx);
  else
    finish (ctx, "lgogdownloader was not found. The GOG Manager AppImage comes with it; "
                 "otherwise install it from your distribution's packages.");
  gtk_window_present (ctx->window);
}
