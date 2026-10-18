/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "dialog_window.h"

#define CAN_CLOSE_KEY "gog-can-close"

static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  (void) user_data;
  /* TRUE stops the close. */
  return g_object_get_data (G_OBJECT (window), CAN_CLOSE_KEY) != NULL;
}

GtkWindow *
gog_dialog_window_new (GtkWindow *parent, const char *title, int width, int height,
                       int min_width, int min_height)
{
  GtkWindow *window = GTK_WINDOW (adw_window_new ());
  gtk_window_set_title (window, title);
  gtk_window_set_transient_for (window, parent);
  gtk_window_set_modal (window, TRUE);
  gtk_window_set_destroy_with_parent (window, TRUE);
  gtk_window_set_resizable (window, TRUE);
  gtk_window_set_default_size (window, width, height);
  gtk_widget_set_size_request (GTK_WIDGET (window), min_width, min_height);

  /* Esc closes it, as it did when these were AdwDialogs. */
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (
      GTK_SHORTCUT_CONTROLLER (shortcuts),
      gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                        gtk_named_action_new ("window.close")));
  gtk_widget_add_controller (GTK_WIDGET (window), shortcuts);

  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), NULL);
  return window;
}

void
gog_dialog_window_set_can_close (GtkWindow *window, gboolean can_close)
{
  /* Stored inverted so a fresh window (no data) can close. */
  g_object_set_data (G_OBJECT (window), CAN_CLOSE_KEY, can_close ? NULL : GINT_TO_POINTER (1));
  gtk_window_set_deletable (window, can_close);
}

/* ---- GogAlert ---- */

typedef struct
{
  gchar *id;
  gchar *label;
  AdwResponseAppearance appearance;
} AlertResponse;

struct _GogAlert
{
  gchar *heading;
  gchar *body;
  GPtrArray *responses; /* AlertResponse* */
  gchar *default_id;
  gchar *close_id;
  GtkWidget *extra_child; /* floating ref taken over */
  GtkWindow *window;
  GogAlertFunc callback;
  gpointer user_data;
  gboolean answered;
};

static void
alert_response_free (AlertResponse *r)
{
  g_free (r->id);
  g_free (r->label);
  g_free (r);
}

static void
gog_alert_free (GogAlert *alert)
{
  g_free (alert->heading);
  g_free (alert->body);
  g_ptr_array_unref (alert->responses);
  g_free (alert->default_id);
  g_free (alert->close_id);
  g_free (alert);
}

GogAlert *
gog_alert_new (const char *heading, const char *body)
{
  GogAlert *alert = g_new0 (GogAlert, 1);
  alert->heading = g_strdup (heading);
  alert->body = g_strdup (body);
  alert->responses = g_ptr_array_new_with_free_func ((GDestroyNotify) alert_response_free);
  return alert;
}

void
gog_alert_add_response (GogAlert *alert, const char *id, const char *label,
                        AdwResponseAppearance appearance)
{
  AlertResponse *r = g_new0 (AlertResponse, 1);
  r->id = g_strdup (id);
  r->label = g_strdup (label);
  r->appearance = appearance;
  g_ptr_array_add (alert->responses, r);
}

void
gog_alert_set_default_response (GogAlert *alert, const char *id)
{
  g_free (alert->default_id);
  alert->default_id = g_strdup (id);
}

void
gog_alert_set_close_response (GogAlert *alert, const char *id)
{
  g_free (alert->close_id);
  alert->close_id = g_strdup (id);
}

void
gog_alert_set_extra_child (GogAlert *alert, GtkWidget *child)
{
  alert->extra_child = child;
}

/* Reports one response, then closes the window. */
static void
alert_answer (GogAlert *alert, const char *id)
{
  if (alert->answered)
    return;
  alert->answered = TRUE;
  if (alert->callback)
    alert->callback (id, alert->user_data);
  gtk_window_destroy (alert->window);
}

static void
on_alert_button_clicked (GtkButton *button, gpointer user_data)
{
  alert_answer (user_data, g_object_get_data (G_OBJECT (button), "gog-response"));
}

static gboolean
on_alert_close_request (GtkWindow *window, gpointer user_data)
{
  (void) window;
  GogAlert *alert = user_data;
  if (!alert->answered)
    {
      alert->answered = TRUE;
      if (alert->callback)
        alert->callback (alert->close_id, alert->user_data);
    }
  return FALSE;
}

static void
on_alert_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
  gog_alert_free (user_data);
}

void
gog_alert_present (GogAlert *alert, GtkWindow *parent, GogAlertFunc callback, gpointer user_data)
{
  alert->callback = callback;
  alert->user_data = user_data;
  if (!alert->close_id && alert->responses->len > 0)
    alert->close_id = g_strdup (((AlertResponse *) g_ptr_array_index (alert->responses, 0))->id);

  GtkWindow *window = gog_dialog_window_new (parent, alert->heading, 440, -1, 320, -1);
  alert->window = window;

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  /* The heading below already says it; keep the title bar for moving. */
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), gtk_label_new (NULL));
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);
  gtk_widget_set_margin_bottom (box, 24);

  GtkWidget *heading = gtk_label_new (alert->heading);
  gtk_widget_add_css_class (heading, "title-2");
  gtk_label_set_wrap (GTK_LABEL (heading), TRUE);
  gtk_label_set_justify (GTK_LABEL (heading), GTK_JUSTIFY_CENTER);
  gtk_box_append (GTK_BOX (box), heading);

  if (alert->body && *alert->body)
    {
      GtkWidget *body = gtk_label_new (alert->body);
      gtk_label_set_wrap (GTK_LABEL (body), TRUE);
      gtk_label_set_selectable (GTK_LABEL (body), TRUE);
      /* Left-aligned: bodies hold lists and file names. */
      gtk_label_set_xalign (GTK_LABEL (body), 0);
      gtk_box_append (GTK_BOX (box), body);
      gtk_accessible_update_relation (GTK_ACCESSIBLE (window), GTK_ACCESSIBLE_RELATION_DESCRIBED_BY,
                                      body, NULL, -1);
    }

  if (alert->extra_child)
    {
      gtk_widget_set_vexpand (alert->extra_child, TRUE);
      gtk_box_append (GTK_BOX (box), alert->extra_child);
      alert->extra_child = NULL;
    }

  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous (GTK_BOX (buttons), TRUE);
  gtk_widget_set_margin_top (buttons, 12);
  GtkWidget *default_button = NULL;
  for (guint i = 0; i < alert->responses->len; i++)
    {
      AlertResponse *r = g_ptr_array_index (alert->responses, i);
      GtkWidget *button = gtk_button_new_with_mnemonic (r->label);
      gtk_widget_add_css_class (button, "pill");
      if (r->appearance == ADW_RESPONSE_SUGGESTED)
        gtk_widget_add_css_class (button, "suggested-action");
      else if (r->appearance == ADW_RESPONSE_DESTRUCTIVE)
        gtk_widget_add_css_class (button, "destructive-action");
      g_object_set_data_full (G_OBJECT (button), "gog-response", g_strdup (r->id), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (on_alert_button_clicked), alert);
      gtk_box_append (GTK_BOX (buttons), button);
      if (g_strcmp0 (r->id, alert->default_id) == 0)
        default_button = button;
    }
  gtk_box_append (GTK_BOX (box), buttons);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), box);
  adw_window_set_content (ADW_WINDOW (window), toolbar_view);

  g_signal_connect (window, "close-request", G_CALLBACK (on_alert_close_request), alert);
  g_signal_connect (window, "destroy", G_CALLBACK (on_alert_destroy), alert);

  if (default_button)
    {
      gtk_window_set_default_widget (window, default_button);
      gtk_window_set_focus (window, default_button);
    }
  gtk_window_present (window);
}
