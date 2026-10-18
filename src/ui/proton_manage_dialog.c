/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "proton_manage_dialog.h"
#include "../proton_fetch.h"
#include "../game.h"
#include "../text_util.h"

typedef struct
{
  AdwDialog *dialog;
  GogConfig *config;
  GogLibrary *library;
  GogState *state;
  GogProtonManageDoneFunc on_done;
  gpointer user_data;

  GtkWidget *list_box;
  GtkWidget *empty_label;
  GtkLabel *status_label;
  GtkButton *remove_selected_button;
  GtkButton *remove_old_button;

  GPtrArray *names;  /* installed build names, newest first */
  GPtrArray *checks; /* GtkCheckButton*, parallel to names */
  GPtrArray *rows;   /* AdwActionRow*, parallel to names */
  GPtrArray *row_notes; /* gchar* subtitle without the size, parallel to names */
  GArray *sizes;     /* guint64 bytes, parallel to names once measured, else NULL */
  GCancellable *measuring;
  GStrv pending;     /* builds being removed */
  gboolean busy;
  gboolean changed;
} ManageCtx;

static void rebuild_list (ManageCtx *ctx);

static gchar *
build_path (const char *name)
{
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  gchar *path = g_build_filename (tools_dir, name, NULL);
  g_free (tools_dir);
  return path;
}

static gboolean
game_uses_runner (GogGame *game, const char *path)
{
  return gog_game_get_installed (game) && gog_game_get_runner_kind (game) == GOG_RUNNER_PROTON &&
         g_strcmp0 (gog_game_get_runner_path (game), path) == 0;
}

/* Titles of installed games (not DLC records, which share their base game's
 * runner) that use any of names. Transfer full; NULL if none. */
static gchar *
titles_using (ManageCtx *ctx, const char *const *names)
{
  GString *out = NULL;
  GListModel *model = gog_library_get_model (ctx->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      if (!gog_game_get_base_game (g))
        for (const char *const *name = names; *name; name++)
          {
            gchar *path = build_path (*name);
            gboolean uses = game_uses_runner (g, path);
            g_free (path);
            if (uses)
              {
                if (!out)
                  out = g_string_new (gog_game_get_title (g));
                else
                  g_string_append_printf (out, ", %s", gog_game_get_title (g));
                break;
              }
          }
      g_object_unref (g);
    }
  return out ? g_string_free (out, FALSE) : NULL;
}

GStrv
gog_proton_unused_old_versions (GogConfig *config, GogLibrary *library)
{
  GPtrArray *names = proton_fetch_list_installed ();
  GStrvBuilder *builder = g_strv_builder_new ();
  GListModel *model = gog_library_get_model (library);
  guint n_games = g_list_model_get_n_items (model);

  for (guint i = 1; i < names->len; i++) /* names[0] is the newest */
    {
      gchar *path = build_path (g_ptr_array_index (names, i));
      gboolean used = config->default_runner_kind == GOG_RUNNER_PROTON &&
                      g_strcmp0 (config->default_runner_path, path) == 0;
      for (guint j = 0; j < n_games && !used; j++)
        {
          GogGame *g = g_list_model_get_item (model, j);
          used = game_uses_runner (g, path);
          g_object_unref (g);
        }
      if (!used)
        g_strv_builder_add (builder, g_ptr_array_index (names, i));
      g_free (path);
    }

  g_ptr_array_unref (names);
  GStrv out = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  return out;
}

/* Total size of names, or 0 when sizes are not known yet. */
static guint64
size_of (ManageCtx *ctx, const char *const *names)
{
  if (!ctx->sizes)
    return 0;
  guint64 total = 0;
  for (guint i = 0; i < ctx->names->len; i++)
    if (g_strv_contains (names, g_ptr_array_index (ctx->names, i)))
      total += g_array_index (ctx->sizes, guint64, i);
  return total;
}

static void
set_status (ManageCtx *ctx, const char *message)
{
  gtk_label_set_text (ctx->status_label, message);
  gtk_widget_set_visible (GTK_WIDGET (ctx->status_label), message && *message);
#if GTK_CHECK_VERSION(4, 14, 0)
  if (message && *message)
    gtk_accessible_announce (GTK_ACCESSIBLE (ctx->dialog), message,
                             GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
#endif
}

static void
update_buttons (ManageCtx *ctx)
{
  gboolean any_checked = FALSE;
  for (guint i = 0; i < ctx->checks->len; i++)
    if (gtk_check_button_get_active (g_ptr_array_index (ctx->checks, i)))
      any_checked = TRUE;
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->remove_selected_button), !ctx->busy && any_checked);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->remove_old_button),
                            !ctx->busy && ctx->names->len > 1);
  gtk_widget_set_sensitive (ctx->list_box, !ctx->busy);
  adw_dialog_set_can_close (ctx->dialog, !ctx->busy);
}

static void
on_check_toggled (GtkCheckButton *check, gpointer user_data)
{
  (void) check;
  update_buttons (user_data);
}

/* The newest build that is not in removed, or NULL. */
static const char *
newest_kept (ManageCtx *ctx, const char *const *removed)
{
  for (guint i = 0; i < ctx->names->len; i++)
    {
      const char *name = g_ptr_array_index (ctx->names, i);
      if (!g_strv_contains (removed, name))
        return name;
    }
  return NULL;
}

/* Points games and the default runner that used a removed build at
 * replacement instead (when there is one). */
static void
move_users_to (ManageCtx *ctx, const char *const *removed, const char *replacement)
{
  if (!replacement)
    return;
  gchar *new_path = build_path (replacement);
  gboolean state_changed = FALSE;

  GListModel *model = gog_library_get_model (ctx->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      for (const char *const *name = removed; *name; name++)
        {
          gchar *path = build_path (*name);
          gboolean uses = game_uses_runner (g, path);
          g_free (path);
          if (uses)
            {
              gog_game_set_runner_path (g, new_path);
              gog_state_update_from_game (ctx->state, g);
              state_changed = TRUE;
              break;
            }
        }
      g_object_unref (g);
    }

  if (state_changed)
    {
      GError *error = NULL;
      if (!gog_state_save (ctx->state, &error))
        {
          g_warning ("Could not save install state: %s", error->message);
          g_clear_error (&error);
        }
    }

  if (ctx->config->default_runner_kind == GOG_RUNNER_PROTON)
    for (const char *const *name = removed; *name; name++)
      {
        gchar *path = build_path (*name);
        gboolean was_default = g_strcmp0 (ctx->config->default_runner_path, path) == 0;
        g_free (path);
        if (was_default)
          {
            g_free (ctx->config->default_runner_path);
            ctx->config->default_runner_path = g_strdup (new_path);
            GError *error = NULL;
            if (!gog_config_save (ctx->config, &error))
              {
                g_warning ("Could not save config: %s", error->message);
                g_clear_error (&error);
              }
            break;
          }
      }

  g_free (new_path);
}

static void
on_remove_finished (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  ManageCtx *ctx = user_data;
  GError *error = NULL;
  gboolean ok = proton_fetch_remove_finish (res, &error);

  /* Even after a partial failure some builds may be gone; recompute what
   * is actually left before moving games over. */
  GPtrArray *left = proton_fetch_list_installed ();
  GStrvBuilder *gone_builder = g_strv_builder_new ();
  for (gchar **name = ctx->pending; *name; name++)
    if (!g_ptr_array_find_with_equal_func (left, *name, g_str_equal, NULL))
      g_strv_builder_add (gone_builder, *name);
  GStrv gone = g_strv_builder_end (gone_builder);
  g_strv_builder_unref (gone_builder);
  g_ptr_array_unref (left);

  guint n_gone = g_strv_length (gone);
  if (n_gone > 0)
    {
      ctx->changed = TRUE;
      move_users_to (ctx, (const char *const *) gone,
                     newest_kept (ctx, (const char *const *) gone));
    }

  ctx->busy = FALSE;
  rebuild_list (ctx);

  if (ok)
    {
      gchar *msg = g_strdup_printf (n_gone == 1 ? "Removed %u version." : "Removed %u versions.",
                                    n_gone);
      set_status (ctx, msg);
      g_free (msg);
    }
  else
    {
      gchar *msg = g_strdup_printf ("Could not remove everything: %s", error->message);
      set_status (ctx, msg);
      g_free (msg);
      g_clear_error (&error);
    }

  g_strfreev (gone);
  g_clear_pointer (&ctx->pending, g_strfreev);
}

static void
start_remove (ManageCtx *ctx)
{
  ctx->busy = TRUE;
  update_buttons (ctx);
  guint n = g_strv_length (ctx->pending);
  gchar *msg = g_strdup_printf (n == 1 ? "Removing %u version…" : "Removing %u versions…", n);
  set_status (ctx, msg);
  g_free (msg);
  proton_fetch_remove_async ((const char *const *) ctx->pending, NULL, on_remove_finished, ctx);
}

static void
on_confirm_response (AdwAlertDialog *alert, const char *response, gpointer user_data)
{
  (void) alert;
  ManageCtx *ctx = user_data;
  if (g_strcmp0 (response, "remove") == 0)
    start_remove (ctx);
  else
    g_clear_pointer (&ctx->pending, g_strfreev);
}

/* Takes ownership of names and asks before removing them. */
static void
confirm_remove (ManageCtx *ctx, GStrv names)
{
  g_clear_pointer (&ctx->pending, g_strfreev);
  ctx->pending = names;
  guint n = g_strv_length (names);

  gchar *heading = n == 1 ? g_strdup_printf ("Remove %s?", names[0])
                          : g_strdup_printf ("Remove %u GE-Proton versions?", n);
  GString *body = g_string_new (NULL);
  if (n > 1)
    {
      gchar *list = g_strjoinv ("\n", names);
      g_string_append_printf (body, "%s\n\n", list);
      g_free (list);
    }
  guint64 freed = size_of (ctx, (const char *const *) names);
  if (freed > 0)
    {
      gchar *freed_str = text_format_size (freed);
      g_string_append_printf (body, "The files are deleted from this computer, freeing %s.",
                              freed_str);
      g_free (freed_str);
    }
  else
    g_string_append (body, "The files are deleted from this computer.");

  gchar *users = titles_using (ctx, (const char *const *) names);
  if (users)
    {
      const char *replacement = newest_kept (ctx, (const char *const *) names);
      if (replacement)
        g_string_append_printf (body, "\n\nThese games will use %s instead: %s.", replacement,
                                users);
      else
        g_string_append_printf (body,
                                "\n\nThese games will not start until you get GE-Proton "
                                "again: %s.",
                                users);
      g_free (users);
    }

  AdwDialog *alert = adw_alert_dialog_new (heading, body->str);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (alert), "cancel", "Cancel", "remove",
                                  "Remove", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (alert), "remove",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (alert), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (alert), "cancel");
  g_signal_connect (alert, "response", G_CALLBACK (on_confirm_response), ctx);
  adw_dialog_present (alert, GTK_WIDGET (ctx->dialog));

  g_free (heading);
  g_string_free (body, TRUE);
}

static void
on_remove_selected_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  ManageCtx *ctx = user_data;
  GStrvBuilder *builder = g_strv_builder_new ();
  for (guint i = 0; i < ctx->checks->len; i++)
    if (gtk_check_button_get_active (g_ptr_array_index (ctx->checks, i)))
      g_strv_builder_add (builder, g_ptr_array_index (ctx->names, i));
  GStrv names = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  if (names[0])
    confirm_remove (ctx, names);
  else
    g_strfreev (names);
}

static void
on_remove_old_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  ManageCtx *ctx = user_data;
  GStrvBuilder *builder = g_strv_builder_new ();
  for (guint i = 1; i < ctx->names->len; i++) /* names[0] is the newest */
    g_strv_builder_add (builder, g_ptr_array_index (ctx->names, i));
  GStrv names = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  if (names[0])
    confirm_remove (ctx, names);
  else
    g_strfreev (names);
}

static void
on_measured (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GError *error = NULL;
  GArray *sizes = proton_fetch_measure_finish (res, &error);
  if (!sizes)
    {
      /* Cancelled: the list was rebuilt or the dialog closed, and user_data
       * may be gone. */
      g_clear_error (&error);
      return;
    }
  ManageCtx *ctx = user_data;
  g_clear_object (&ctx->measuring);
  if (sizes->len != ctx->names->len)
    {
      g_array_unref (sizes);
      return;
    }
  g_clear_pointer (&ctx->sizes, g_array_unref);
  ctx->sizes = sizes;
  for (guint i = 0; i < ctx->rows->len; i++)
    {
      gchar *size_str = text_format_size (g_array_index (sizes, guint64, i));
      gchar *subtitle = g_strdup_printf ("%s · %s", size_str,
                                         (const char *) g_ptr_array_index (ctx->row_notes, i));
      adw_action_row_set_subtitle (g_ptr_array_index (ctx->rows, i), subtitle);
      g_free (subtitle);
      g_free (size_str);
    }
}

static void
rebuild_list (ManageCtx *ctx)
{
  if (ctx->measuring)
    {
      g_cancellable_cancel (ctx->measuring);
      g_clear_object (&ctx->measuring);
    }
  g_clear_pointer (&ctx->sizes, g_array_unref);

  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (ctx->list_box)))
    gtk_list_box_remove (GTK_LIST_BOX (ctx->list_box), child);
  g_ptr_array_set_size (ctx->checks, 0);
  g_ptr_array_set_size (ctx->rows, 0);
  g_ptr_array_set_size (ctx->row_notes, 0);
  g_ptr_array_unref (ctx->names);
  ctx->names = proton_fetch_list_installed ();

  for (guint i = 0; i < ctx->names->len; i++)
    {
      const char *name = g_ptr_array_index (ctx->names, i);
      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);

      const char *one[] = { name, NULL };
      gchar *users = titles_using (ctx, one);
      gchar *note = NULL;
      if (i == 0 && users)
        note = g_strdup_printf ("Newest. Used by %s", users);
      else if (i == 0)
        note = g_strdup ("Newest");
      else if (users)
        note = g_strdup_printf ("Used by %s", users);
      else
        note = g_strdup ("Not used by any game");
      adw_action_row_set_subtitle (row, note);
      g_ptr_array_add (ctx->rows, row);
      g_ptr_array_add (ctx->row_notes, note);
      g_free (users);

      GtkWidget *check = gtk_check_button_new ();
      gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
      gchar *label = g_strdup_printf ("Select %s", name);
      gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label, -1);
      g_free (label);
      g_signal_connect (check, "toggled", G_CALLBACK (on_check_toggled), ctx);
      adw_action_row_add_prefix (row, check);
      adw_action_row_set_activatable_widget (row, check);
      g_ptr_array_add (ctx->checks, check);

      gtk_list_box_append (GTK_LIST_BOX (ctx->list_box), GTK_WIDGET (row));
    }

  gtk_widget_set_visible (ctx->list_box, ctx->names->len > 0);
  gtk_widget_set_visible (ctx->empty_label, ctx->names->len == 0);
  update_buttons (ctx);

  if (ctx->names->len > 0)
    {
      ctx->measuring = g_cancellable_new ();
      g_ptr_array_add (ctx->names, NULL);
      proton_fetch_measure_async ((const char *const *) ctx->names->pdata, ctx->measuring,
                                  on_measured, ctx);
      g_ptr_array_remove_index (ctx->names, ctx->names->len - 1);
    }
}

static void
on_dialog_closed (AdwDialog *dialog, gpointer user_data)
{
  (void) dialog;
  ManageCtx *ctx = user_data;
  if (ctx->on_done)
    ctx->on_done (ctx->changed, ctx->user_data);
  if (ctx->measuring)
    {
      g_cancellable_cancel (ctx->measuring);
      g_object_unref (ctx->measuring);
    }
  g_clear_pointer (&ctx->sizes, g_array_unref);
  g_ptr_array_unref (ctx->names);
  g_ptr_array_unref (ctx->checks);
  g_ptr_array_unref (ctx->rows);
  g_ptr_array_unref (ctx->row_notes);
  g_strfreev (ctx->pending);
  g_free (ctx);
}

void
gog_show_proton_manage_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                                GogState *state, GogProtonManageDoneFunc on_done,
                                gpointer user_data)
{
  ManageCtx *ctx = g_new0 (ManageCtx, 1);
  ctx->config = config;
  ctx->library = library;
  ctx->state = state;
  ctx->on_done = on_done;
  ctx->user_data = user_data;
  ctx->names = g_ptr_array_new_with_free_func (g_free);
  ctx->checks = g_ptr_array_new ();
  ctx->rows = g_ptr_array_new ();
  ctx->row_notes = g_ptr_array_new_with_free_func (g_free);

  AdwDialog *dialog = adw_dialog_new ();
  ctx->dialog = dialog;
  adw_dialog_set_title (dialog, "Manage GE-Proton");
  adw_dialog_set_content_width (dialog, 480);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start (outer, 18);
  gtk_widget_set_margin_end (outer, 18);
  gtk_widget_set_margin_top (outer, 12);
  gtk_widget_set_margin_bottom (outer, 18);

  GtkWidget *desc = gtk_label_new (
      "GE-Proton versions downloaded with Get GE-Proton. Choose the ones to remove. "
      "Versions from Steam or Heroic are not listed here.");
  gtk_label_set_wrap (GTK_LABEL (desc), TRUE);
  gtk_label_set_xalign (GTK_LABEL (desc), 0);
  gtk_widget_add_css_class (desc, "dim-label");
  gtk_box_append (GTK_BOX (outer), desc);

  ctx->list_box = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (ctx->list_box), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (ctx->list_box, "boxed-list");
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->list_box), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  "Downloaded GE-Proton versions", -1);
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 360);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), ctx->list_box);
  gtk_box_append (GTK_BOX (outer), scroll);

  ctx->empty_label = gtk_label_new ("No GE-Proton versions have been downloaded yet.");
  gtk_label_set_wrap (GTK_LABEL (ctx->empty_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (ctx->empty_label), 0);
  gtk_box_append (GTK_BOX (outer), ctx->empty_label);

  ctx->status_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (ctx->status_label, TRUE);
  gtk_label_set_xalign (ctx->status_label, 0);
  gtk_widget_set_visible (GTK_WIDGET (ctx->status_label), FALSE);
  gtk_box_append (GTK_BOX (outer), GTK_WIDGET (ctx->status_label));

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  ctx->remove_old_button = GTK_BUTTON (gtk_button_new_with_label ("Remove All Except Newest…"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->remove_old_button), "destructive-action");
  g_signal_connect (ctx->remove_old_button, "clicked", G_CALLBACK (on_remove_old_clicked), ctx);
  ctx->remove_selected_button = GTK_BUTTON (gtk_button_new_with_label ("Remove Selected…"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->remove_selected_button), "destructive-action");
  g_signal_connect (ctx->remove_selected_button, "clicked",
                    G_CALLBACK (on_remove_selected_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->remove_old_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->remove_selected_button));
  gtk_box_append (GTK_BOX (outer), btn_box);

  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), outer);
  adw_dialog_set_child (dialog, toolbar_view);

  rebuild_list (ctx);

  g_signal_connect (dialog, "closed", G_CALLBACK (on_dialog_closed), ctx);
  adw_dialog_present (dialog, GTK_WIDGET (parent));
}
