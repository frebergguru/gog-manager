/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "login_dialog.h"
#include "dialog_window.h"
#include "../gog_sync.h"
#include "../config.h"
#include <string.h>
#ifdef HAVE_WEBKIT
#include <webkit/webkit.h>
#endif

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
  GtkStack *ways;          /* "web" (GOG's page in here) | "browser" (copy and paste)
                            * | "done" (logging in, then the result) */
  AdwStatusPage *done_page;
  GtkWidget *intro;
  GtkWidget *web_view;     /* NULL without WebKit */
  GtkWidget *browser_button; /* "Use my web browser instead" */
  gboolean submitted;      /* the address went to lgogdownloader */
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

/* GOG's page goes once it has done its part: it is no use any more, and
 * its web processes end with it. Not from inside the view's own signal
 * handlers, so from an idle. */
static gboolean
drop_web_view (gpointer data)
{
  LoginCtx *ctx = data;
  if (!returned (ctx))
    return G_SOURCE_REMOVE;
  GtkWidget *web = gtk_stack_get_child_by_name (ctx->ways, "web");
  if (web)
    gtk_stack_remove (ctx->ways, web);
  ctx->web_view = NULL;
  return G_SOURCE_REMOVE;
}

/* The window shows only how it went (or that it's going): the login page,
 * the steps and their buttons are done with. */
static void
show_done (LoginCtx *ctx, const char *icon, const char *title, const char *text, gboolean busy)
{
  adw_status_page_set_icon_name (ctx->done_page, icon);
  adw_status_page_set_title (ctx->done_page, title);
  adw_status_page_set_description (ctx->done_page, text);
  /* A spinner while logging in, unless motion is turned down: then the
   * title says it. */
  gboolean animate = TRUE;
  g_object_get (gtk_settings_get_default (), "gtk-enable-animations", &animate, NULL);
  adw_status_page_set_child (ctx->done_page, busy && animate ? adw_spinner_new () : NULL);
  gtk_stack_set_visible_child_name (ctx->ways, "done");
  gtk_widget_set_visible (ctx->intro, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (ctx->status), FALSE);
  gtk_widget_set_visible (ctx->browser_button, FALSE);
  gtk_widget_set_visible (ctx->login_button, FALSE);
  /* Close stays at the right, without the button that pushed it there. */
  gtk_widget_set_halign (gtk_widget_get_parent (ctx->close_button), GTK_ALIGN_END);
  if (ctx->web_view)
    {
      ctx->pending++;
      g_idle_add (drop_web_view, ctx);
    }
  gchar *spoken = text ? g_strdup_printf ("%s. %s", title, text) : g_strdup (title);
  gog_announce (GTK_WIDGET (ctx->window), spoken);
  g_free (spoken);
}

/* The end, whichever way: ok says whether lgogdownloader is logged in. */
static void
finish (LoginCtx *ctx, gboolean ok, const char *title, const char *text)
{
  show_done (ctx, ok ? "object-select-symbolic" : "dialog-warning-symbolic", title, text, FALSE);
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
      if (ctx->logged_in)
        finish (ctx, TRUE, "You’re Logged In to GOG",
                "GOG Manager can now check for new games and updates, and download them.");
      else
        finish (ctx, FALSE, "The Login Didn’t Work",
                "lgogdownloader is not logged in. Close this window and try Log In to GOG "
                "again.");
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
      finish (ctx, FALSE, "Could Not Check the Login",
              "lgogdownloader could not be run to check it.");
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
static void show_login_page (LoginCtx *ctx);

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
      show_login_page (ctx);
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
  if (gog_config_launch_dir ())
    g_subprocess_launcher_set_cwd (launcher, gog_config_launch_dir ());
  GError *error = NULL;
  ctx->proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, &error);
  g_object_unref (launcher);
  g_free (argv);
  if (!ctx->proc)
    {
      gchar *text = g_strdup_printf ("lgogdownloader could not be run: %s", error->message);
      finish (ctx, FALSE, "Could Not Start the Login", text);
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

/* Hands GOG's final address to lgogdownloader, which turns its code into a
 * login. FALSE (and says why) when it isn't that address. */
static gboolean
submit_address (LoginCtx *ctx, const char *text)
{
  if (ctx->submitted || !ctx->proc)
    return FALSE;
  gchar *address = g_strstrip (g_strdup (text));
  if (!g_str_has_prefix (address, SUCCESS_PREFIX) || !strstr (address, "code="))
    {
      set_status (ctx, "That isn't the address GOG ends the login on. It starts with "
                       SUCCESS_PREFIX " and has a code in it; copy it from the browser's "
                       "address bar once the login is done.",
                  TRUE);
      g_free (address);
      return FALSE;
    }
  gchar *line = g_strconcat (address, "\n", NULL);
  GOutputStream *in = g_subprocess_get_stdin_pipe (ctx->proc);
  gboolean ok = g_output_stream_write_all (in, line, strlen (line), NULL, NULL, NULL) &&
                g_output_stream_close (in, NULL, NULL);
  g_free (line);
  g_free (address);
  ctx->submitted = TRUE;
  gtk_widget_set_sensitive (ctx->login_button, FALSE);
  gtk_widget_set_sensitive (ctx->entry, FALSE);
  gtk_widget_set_sensitive (ctx->browser_button, FALSE);
  if (!ok)
    finish (ctx, FALSE, "Could Not Finish the Login",
            "The address could not be passed on to lgogdownloader.");
  else
    show_done (ctx, NULL, "Logging In…", "lgogdownloader is finishing the login with GOG.",
               TRUE);
  return ok;
}

static void
on_login_clicked (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  LoginCtx *ctx = user_data;
  if (!submit_address (ctx, gtk_editable_get_text (GTK_EDITABLE (ctx->entry))))
    gtk_widget_grab_focus (ctx->entry);
}

/* The copy-and-paste way: GOG's page in the web browser. */
static void
use_browser (LoginCtx *ctx, const char *why)
{
  gtk_stack_set_visible_child_name (ctx->ways, "browser");
  gtk_widget_set_visible (ctx->browser_button, FALSE);
  gtk_widget_set_visible (ctx->login_button, TRUE);
  gtk_window_set_default_size (ctx->window, 560, 440);
  if (ctx->login_url)
    {
      gtk_widget_set_sensitive (ctx->open_button, TRUE);
      gtk_widget_grab_focus (ctx->open_button);
    }
  if (why)
    set_status (ctx, why, TRUE);
}

static void
on_browser_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  use_browser (user_data, NULL);
}

#ifdef HAVE_WEBKIT
/* GOG's page in here: once GOG sends it to the final address, that address
 * goes to lgogdownloader by itself. Only https pages load, and nothing is
 * kept afterwards (an ephemeral session): lgogdownloader keeps the login. */
static gboolean
on_decide_policy (WebKitWebView *view, WebKitPolicyDecision *decision,
                  WebKitPolicyDecisionType type, gpointer user_data)
{
  LoginCtx *ctx = user_data;
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    return FALSE;
  WebKitNavigationAction *action =
      webkit_navigation_policy_decision_get_navigation_action (WEBKIT_NAVIGATION_POLICY_DECISION (decision));
  const char *uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));
  if (uri && g_str_has_prefix (uri, SUCCESS_PREFIX))
    {
      webkit_policy_decision_ignore (decision);
      gtk_widget_set_sensitive (ctx->web_view, FALSE);
      submit_address (ctx, uri);
      return TRUE;
    }
  if (!uri || !g_str_has_prefix (uri, "https://"))
    {
      webkit_policy_decision_ignore (decision);
      return TRUE;
    }
  if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
    {
      /* A page that wants a window of its own opens in this one. */
      webkit_policy_decision_ignore (decision);
      webkit_web_view_load_uri (view, uri);
      return TRUE;
    }
  return FALSE;
}

static gboolean
on_load_failed (WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error,
                gpointer user_data)
{
  (void) view;
  (void) event;
  LoginCtx *ctx = user_data;
  /* An ignored navigation (the final address) ends the load like this too. */
  if (ctx->submitted || (uri && g_str_has_prefix (uri, SUCCESS_PREFIX)) ||
      g_error_matches (error, WEBKIT_POLICY_ERROR, WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE))
    return FALSE;
  gchar *why = g_strdup_printf ("GOG's login page did not load here (%s). Log in in your web "
                                "browser instead:", error->message);
  use_browser (ctx, why);
  g_free (why);
  return FALSE;
}

static void
on_web_process_terminated (WebKitWebView *view, WebKitWebProcessTerminationReason reason,
                           gpointer user_data)
{
  (void) view;
  (void) reason;
  LoginCtx *ctx = user_data;
  if (!ctx->submitted && !ctx->gone)
    use_browser (ctx, "The login page stopped working here. Log in in your web browser "
                      "instead:");
}

/* In the AppImage, WebKit looks for its helper programs relative to the
 * current folder (see packaging/build-appimage.sh), and aborts the whole
 * app when they aren't there. Should they not be, the login goes the
 * copy-and-paste way instead. */
static gboolean
webkit_helpers_found (void)
{
  if (!g_getenv ("APPDIR"))
    return TRUE;
  return g_file_test ("lib/webkitgtk-6.0/WebKitWebProcess", G_FILE_TEST_IS_EXECUTABLE) &&
         g_file_test ("lib/webkitgtk-6.0/WebKitNetworkProcess", G_FILE_TEST_IS_EXECUTABLE);
}

static GtkWidget *
make_web_view (LoginCtx *ctx)
{
  if (!webkit_helpers_found ())
    return NULL;
  WebKitNetworkSession *session = webkit_network_session_new_ephemeral ();
  GtkWidget *view = g_object_new (WEBKIT_TYPE_WEB_VIEW, "network-session", session, NULL);
  g_object_unref (session);
  gtk_widget_set_vexpand (view, TRUE);
  gtk_widget_set_hexpand (view, TRUE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "GOG login page", -1);
  g_signal_connect (view, "decide-policy", G_CALLBACK (on_decide_policy), ctx);
  g_signal_connect (view, "load-failed", G_CALLBACK (on_load_failed), ctx);
  g_signal_connect (view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), ctx);
  return view;
}
#endif

/* lgogdownloader gave GOG's login page: show it. */
static void
show_login_page (LoginCtx *ctx)
{
#ifdef HAVE_WEBKIT
  if (ctx->web_view && g_strcmp0 (gtk_stack_get_visible_child_name (ctx->ways), "web") == 0)
    {
      webkit_web_view_load_uri (WEBKIT_WEB_VIEW (ctx->web_view), ctx->login_url);
      set_status (ctx, "Log in to GOG on the page above.", FALSE);
      gtk_widget_grab_focus (ctx->web_view);
      return;
    }
#endif
  gtk_widget_set_sensitive (ctx->open_button, TRUE);
  set_status (ctx, "Ready. Open GOG's login page to begin.", FALSE);
  gtk_widget_grab_focus (ctx->open_button);
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
  /* Room for GOG's page when it's shown in here. */
#ifdef HAVE_WEBKIT
  ctx->window = gog_dialog_window_new (parent, "Log In to GOG", 760, 820, 360, 420);
#else
  ctx->window = gog_dialog_window_new (parent, "Log In to GOG", 560, 440, 360, 320);
#endif
  g_signal_connect (ctx->window, "destroy", G_CALLBACK (on_destroy), ctx);

  GtkWidget *view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (view), adw_header_bar_new ());
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 18);
  gtk_widget_set_margin_end (box, 18);
  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 18);

  GtkWidget *intro = ctx->intro = gtk_label_new (
      "GOG Manager downloads your games with lgogdownloader, which needs to be logged in to "
      "your GOG account once. You log in on GOG's own login page; GOG Manager never sees your "
      "password.");
  gtk_label_set_wrap (GTK_LABEL (intro), TRUE);
  gtk_label_set_xalign (GTK_LABEL (intro), 0.0);
  gtk_box_append (GTK_BOX (box), intro);

  ctx->ways = GTK_STACK (gtk_stack_new ());
  gtk_widget_set_vexpand (GTK_WIDGET (ctx->ways), TRUE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->ways));

  /* The copy-and-paste way. */
  GtkWidget *browser = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *step1 = gtk_label_new ("1. Open GOG's login page in your web browser and log in "
                                    "there.");
  gtk_label_set_wrap (GTK_LABEL (step1), TRUE);
  gtk_label_set_xalign (GTK_LABEL (step1), 0.0);
  gtk_widget_add_css_class (step1, "heading");
  gtk_box_append (GTK_BOX (browser), step1);
  ctx->open_button = gtk_button_new_with_label ("Open GOG Login Page");
  gtk_widget_add_css_class (ctx->open_button, "pill");
  gtk_widget_add_css_class (ctx->open_button, "suggested-action");
  gtk_widget_set_halign (ctx->open_button, GTK_ALIGN_START);
  gtk_widget_set_sensitive (ctx->open_button, FALSE);
  g_signal_connect (ctx->open_button, "clicked", G_CALLBACK (on_open_clicked), ctx);
  gtk_box_append (GTK_BOX (browser), ctx->open_button);

  GtkWidget *step2 = gtk_label_new (
      "2. When GOG is done, the browser shows a page whose address starts with "
      "https://embed.gog.com/on_login_success (it may look empty). Copy that whole address "
      "from the address bar and paste it here:");
  gtk_label_set_wrap (GTK_LABEL (step2), TRUE);
  gtk_label_set_xalign (GTK_LABEL (step2), 0.0);
  gtk_widget_add_css_class (step2, "heading");
  gtk_box_append (GTK_BOX (browser), step2);
  ctx->entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->entry), SUCCESS_PREFIX "?…&code=…");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->entry), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Address of the page GOG ends the login on", -1);
  g_signal_connect (ctx->entry, "changed", G_CALLBACK (on_entry_changed), ctx);
  g_signal_connect (ctx->entry, "activate", G_CALLBACK (on_login_clicked), ctx);
  gtk_box_append (GTK_BOX (browser), ctx->entry);
  gtk_stack_add_named (ctx->ways, browser, "browser");

#ifdef HAVE_WEBKIT
  /* GOG's page right here, when WebKit is there: nothing to copy. */
  ctx->web_view = make_web_view (ctx);
  if (ctx->web_view)
    {
      GtkWidget *frame = gtk_frame_new (NULL);
      gtk_frame_set_child (GTK_FRAME (frame), ctx->web_view);
      gtk_stack_add_named (ctx->ways, frame, "web");
      gtk_stack_set_visible_child_name (ctx->ways, "web");
    }
#endif

  ctx->done_page = ADW_STATUS_PAGE (adw_status_page_new ());
  gtk_stack_add_named (ctx->ways, GTK_WIDGET (ctx->done_page), "done");

  ctx->status = GTK_LABEL (gtk_label_new ("Starting lgogdownloader…"));
  gtk_label_set_wrap (ctx->status, TRUE);
  gtk_label_set_xalign (ctx->status, 0.0);
  gtk_widget_add_css_class (GTK_WIDGET (ctx->status), "dim-label");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (ctx->status));

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  ctx->browser_button = gtk_button_new_with_label ("Use My Web Browser Instead");
  gtk_widget_add_css_class (ctx->browser_button, "flat");
  gtk_widget_set_tooltip_text (ctx->browser_button,
                               "Log in on GOG's page in your own web browser, and paste the "
                               "address it ends on");
  gtk_widget_set_hexpand (ctx->browser_button, TRUE);
  gtk_widget_set_halign (ctx->browser_button, GTK_ALIGN_START);
  gtk_widget_set_visible (ctx->browser_button, ctx->web_view != NULL);
  g_signal_connect (ctx->browser_button, "clicked", G_CALLBACK (on_browser_clicked), ctx);
  ctx->close_button = gtk_button_new_with_label ("Cancel");
  g_signal_connect (ctx->close_button, "clicked", G_CALLBACK (on_close_clicked), ctx);
  ctx->login_button = gtk_button_new_with_label ("Log In");
  gtk_widget_add_css_class (ctx->login_button, "suggested-action");
  gtk_widget_set_sensitive (ctx->login_button, FALSE);
  /* Only the copy-and-paste way has an address to send. */
  gtk_widget_set_visible (ctx->login_button, ctx->web_view == NULL);
  g_signal_connect (ctx->login_button, "clicked", G_CALLBACK (on_login_clicked), ctx);
  gtk_box_append (GTK_BOX (buttons), ctx->browser_button);
  gtk_box_append (GTK_BOX (buttons), ctx->close_button);
  gtk_box_append (GTK_BOX (buttons), ctx->login_button);
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (view), box);
  adw_window_set_content (ADW_WINDOW (ctx->window), view);

  if (gog_sync_is_available ())
    start_login (ctx);
  else
    finish (ctx, FALSE, "lgogdownloader Is Missing",
            "lgogdownloader was not found. The GOG Manager AppImage comes with it; "
                 "otherwise install it from your distribution's packages.");
  gtk_window_present (ctx->window);
}
