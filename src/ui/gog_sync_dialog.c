/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gog_sync_dialog.h"
#include "../gog_sync.h"

#define N_SLOTS 8 /* the most downloads at the same time */
#define N_PARTS 3

static const GogSyncPart parts[N_PARTS] = { GOG_SYNC_PART_GAME, GOG_SYNC_PART_DLC,
                                            GOG_SYNC_PART_EXTRA };
static const char *const part_titles[N_PARTS] = { "Game", "DLC", "Extras" };

typedef struct _SyncCtx SyncCtx;

/* One listed game: whether to download it, which of its parts, and in
 * which languages. */
typedef struct
{
  SyncCtx *ctx;
  GogSyncItem *item;
  guint languages; /* 0 = all */
  AdwExpanderRow *row; /* NULL when the files are not known; then plain_row */
  AdwActionRow *plain_row;
  GtkCheckButton *check;
  AdwActionRow *part_rows[N_PARTS];
  GtkCheckButton *part_checks[N_PARTS];
  AdwActionRow *language_row;
  GtkCheckButton *all_languages;
  GPtrArray *language_checks; /* GtkCheckButton*, each with its "bit" */
  GPtrArray *file_rows;       /* AdwActionRow*, each with its "file" */
  gboolean updating;
} GameRow;

typedef struct
{
  GtkWidget *box;
  GtkLabel *name;
  GtkProgressBar *bar;
} SlotView;

struct _SyncCtx
{
  GtkWindow *window; /* a real window, so it can be resized */
  gchar *library_dir;
  GogLibrary *library;
  GogSyncDoneFunc on_done;
  gpointer user_data;

  GtkLabel *summary_label;
  GtkWidget *results_scroll;
  GtkWidget *results_box;
  GtkWidget *progress_box;
  GtkProgressBar *progress_bar;
  GtkWidget *files_heading;
  SlotView slots[N_SLOTS];
  AdwSpinRow *threads_row;
  GtkWidget *log_scroll;
  GtkTextView *log_view;
  GtkButton *check_button;
  GtkButton *select_all_button;
  GtkButton *download_button;
  GtkButton *cancel_button;

  GPtrArray *items; /* GogSyncItem*, what is listed */
  GPtrArray *games; /* GameRow*, in display order */

  GogSyncJob *job;
  gboolean busy;
  gboolean downloaded;
};

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
  g_ptr_array_unref (ctx->games);
  g_ptr_array_unref (ctx->items);
  g_free (ctx->library_dir);
  g_free (ctx);
}

static void
announce (SyncCtx *ctx, const char *message)
{
#if GTK_CHECK_VERSION(4, 14, 0)
  gtk_accessible_announce (GTK_ACCESSIBLE (ctx->window), message,
                           GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
#else
  (void) ctx;
  (void) message;
#endif
}

static void
append_log_line (SyncCtx *ctx, const char *line)
{
  gtk_widget_set_visible (ctx->log_scroll, TRUE);
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

static void
game_row_free (GameRow *game)
{
  g_ptr_array_unref (game->language_checks);
  g_ptr_array_unref (game->file_rows);
  g_free (game);
}

static GogSyncPart
chosen_parts (GameRow *game)
{
  GogSyncPart chosen = 0;
  if (!game->row) /* files not known: all of it */
    return GOG_SYNC_PART_ALL;
  for (guint p = 0; p < N_PARTS; p++)
    if (gtk_widget_get_visible (GTK_WIDGET (game->part_rows[p])) &&
        gtk_check_button_get_active (game->part_checks[p]))
      chosen |= parts[p];
  return chosen;
}

static gboolean
any_checked (SyncCtx *ctx)
{
  for (guint i = 0; i < ctx->games->len; i++)
    {
      GameRow *game = g_ptr_array_index (ctx->games, i);
      if (gtk_check_button_get_active (game->check) && chosen_parts (game))
        return TRUE;
    }
  return FALSE;
}

static void
set_busy (SyncCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->check_button), !busy);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->select_all_button), !busy && ctx->items->len > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), !busy && any_checked (ctx));
  gtk_widget_set_sensitive (ctx->results_box, !busy);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->threads_row), !busy);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), busy);
  /* Closing mid-run would orphan the job; use Cancel instead (see
   * on_close_request). */
  ctx->busy = busy;
}

static void
update_download_button (SyncCtx *ctx)
{
  if (!ctx->job)
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), any_checked (ctx));
}

static void
on_check_toggled (GtkCheckButton *check, gpointer user_data)
{
  (void) check;
  update_download_button (user_data);
}

/* "3 files, 1.2 GB" */
static gchar *
files_text (guint n, guint64 size)
{
  gchar *count = g_strdup_printf (n == 1 ? "%u file" : "%u files", n);
  if (!size)
    return count;
  gchar *bytes = g_format_size (size);
  gchar *out = g_strdup_printf ("%s, %s", count, bytes);
  g_free (bytes);
  g_free (count);
  return out;
}

/* Adds name to a ", "-separated list once. */
static void
add_name (GString *names, GHashTable *seen, const char *name)
{
  if (!name || !g_hash_table_add (seen, (gpointer) name))
    return;
  g_string_append_printf (names, "%s%s", names->len ? ", " : "", name);
}

/* Counts and names per part, for the files the chosen languages take. */
static void
refresh_game (GameRow *game)
{
  if (!game->row)
    {
      gchar *subtitle = g_strdup_printf (game->item->n_files == 1 ? "%u file to download"
                                                                  : "%u files to download",
                                         game->item->n_files);
      adw_action_row_set_subtitle (game->plain_row, subtitle);
      g_free (subtitle);
      update_download_button (game->ctx);
      return;
    }
  for (guint i = 0; i < game->file_rows->len; i++)
    {
      GtkWidget *row = g_ptr_array_index (game->file_rows, i);
      gtk_widget_set_visible (row, gog_sync_file_wanted (g_object_get_data (G_OBJECT (row), "file"),
                                                         game->languages));
    }
  guint total_n = 0;
  guint64 total_size = 0;
  GString *summary = g_string_new (NULL);
  for (guint p = 0; p < N_PARTS; p++)
    {
      guint n = 0;
      guint64 size = 0;
      GString *names = g_string_new (NULL);
      GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
      for (guint f = 0; f < game->item->files->len; f++)
        {
          GogSyncFile *file = g_ptr_array_index (game->item->files, f);
          if (file->part != parts[p] || !gog_sync_file_wanted (file, game->languages))
            continue;
          n++;
          size += file->size;
          if (parts[p] == GOG_SYNC_PART_DLC)
            add_name (names, seen, file->dlc_title ? file->dlc_title : file->gamename);
          else if (parts[p] == GOG_SYNC_PART_EXTRA)
            add_name (names, seen, file->label ? file->label : file->filename);
        }
      gtk_widget_set_visible (GTK_WIDGET (game->part_rows[p]), n > 0);
      if (n > 0)
        {
          gchar *count = files_text (n, size);
          gchar *subtitle = names->len ? g_strdup_printf ("%s — %s", names->str, count)
                                       : g_strdup (count);
          adw_action_row_set_subtitle (game->part_rows[p], subtitle);
          g_string_append_printf (summary, "%s%s: %u", summary->len ? " · " : "",
                                  part_titles[p], n);
          g_free (subtitle);
          g_free (count);
        }
      total_n += n;
      total_size += size;
      g_hash_table_unref (seen);
      g_string_free (names, TRUE);
    }

  gchar *subtitle;
  if (total_n == 0)
    subtitle = g_strdup ("Nothing to download in the chosen languages");
  else
    {
      gchar *bytes = total_size ? g_format_size (total_size) : NULL;
      subtitle = bytes ? g_strdup_printf ("%s — %s", summary->str, bytes) : g_strdup (summary->str);
      g_free (bytes);
    }
  adw_expander_row_set_subtitle (game->row, subtitle);
  g_free (subtitle);
  g_string_free (summary, TRUE);

  if (game->language_row)
    {
      gchar *langs = gog_sync_describe_languages (game->languages);
      adw_action_row_set_subtitle (game->language_row, langs);
      g_free (langs);
    }
  update_download_button (game->ctx);
}

static void
on_part_toggled (GtkCheckButton *check, gpointer user_data)
{
  (void) check;
  GameRow *game = user_data;
  /* Choosing a part means wanting the game. */
  if (gtk_check_button_get_active (check) && !gtk_check_button_get_active (game->check))
    gtk_check_button_set_active (game->check, TRUE);
  update_download_button (game->ctx);
}

static void
on_language_toggled (GtkCheckButton *check, gpointer user_data)
{
  GameRow *game = user_data;
  if (game->updating)
    return;
  game->updating = TRUE;
  if (check == game->all_languages)
    {
      /* "All languages" clears the others; it can't be turned off on its own. */
      gtk_check_button_set_active (check, TRUE);
      for (guint i = 0; i < game->language_checks->len; i++)
        gtk_check_button_set_active (g_ptr_array_index (game->language_checks, i), FALSE);
      game->languages = 0;
    }
  else
    {
      game->languages = 0;
      for (guint i = 0; i < game->language_checks->len; i++)
        {
          GtkCheckButton *c = g_ptr_array_index (game->language_checks, i);
          if (gtk_check_button_get_active (c))
            game->languages |= GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (c), "bit"));
        }
      gtk_check_button_set_active (game->all_languages, game->languages == 0);
    }
  game->updating = FALSE;
  gog_sync_set_game_languages (game->item->gamename, game->languages);
  refresh_game (game);
}

static GtkCheckButton *
language_check (GameRow *game, GtkWidget *box, const char *name, gboolean active)
{
  GtkWidget *check = gtk_check_button_new_with_label (name);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (check), active);
  g_signal_connect (check, "toggled", G_CALLBACK (on_language_toggled), game);
  gtk_box_append (GTK_BOX (box), check);
  return GTK_CHECK_BUTTON (check);
}

/* A "Languages" row with a menu of the languages the game comes in. */
static void
add_language_row (GameRow *game, const char *title)
{
  guint n_langs;
  const GogSyncLanguage *langs = gog_sync_get_languages (&n_langs);
  guint offered = game->item->languages_offered;
  if ((offered & (offered - 1)) == 0) /* one language, or not known */
    return;

  game->language_row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (game->language_row), "Languages");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  game->all_languages = language_check (game, box, "All languages", game->languages == 0);
  for (guint i = 0; i < n_langs; i++)
    if (offered & (1u << i))
      {
        GtkCheckButton *c = language_check (game, box, langs[i].name,
                                            (game->languages & (1u << i)) != 0);
        g_object_set_data (G_OBJECT (c), "bit", GUINT_TO_POINTER (1u << i));
        g_ptr_array_add (game->language_checks, c);
      }
  GtkWidget *scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER,
                                  GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroll), 360);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), box);
  GtkWidget *popover = gtk_popover_new ();
  gtk_popover_set_child (GTK_POPOVER (popover), scroll);

  GtkWidget *button = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (button), "Choose…");
  gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
  gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
  gchar *label = g_strdup_printf ("Choose languages for %s", title);
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                  -1);
  g_free (label);
  adw_action_row_add_suffix (game->language_row, button);
  adw_action_row_set_activatable_widget (game->language_row, button);
  adw_expander_row_add_row (game->row, GTK_WIDGET (game->language_row));
}

/* One file, under its group: what it is, its name, size and language. */
static void
add_file_row (GameRow *game, GogSyncFile *file)
{
  AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  const char *title = file->part == GOG_SYNC_PART_DLC && file->dlc_title ? file->dlc_title
                      : file->label                                       ? file->label
                                                                          : file->filename;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  GString *subtitle = g_string_new (NULL);
  if (g_strcmp0 (title, file->filename) != 0)
    g_string_append (subtitle, file->filename);
  if (file->size)
    {
      gchar *size = g_format_size (file->size);
      g_string_append_printf (subtitle, "%s%s", subtitle->len ? " · " : "", size);
      g_free (size);
    }
  if (file->languages && file->part != GOG_SYNC_PART_EXTRA)
    {
      gchar *langs = gog_sync_describe_languages (file->languages);
      g_string_append_printf (subtitle, "%s%s", subtitle->len ? " · " : "", langs);
      g_free (langs);
    }
  adw_action_row_set_subtitle (row, subtitle->str);
  g_string_free (subtitle, TRUE);
  gtk_widget_add_css_class (GTK_WIDGET (row), "gog-file-row");
  g_object_set_data (G_OBJECT (row), "file", file);
  g_ptr_array_add (game->file_rows, row);
  adw_expander_row_add_row (game->row, GTK_WIDGET (row));
}

static void
add_game_row (SyncCtx *ctx, GtkWidget *list, GogSyncItem *item)
{
  GameRow *game = g_new0 (GameRow, 1);
  game->ctx = ctx;
  game->item = item;
  game->languages = gog_sync_item_languages (item);
  game->language_checks = g_ptr_array_new ();
  game->file_rows = g_ptr_array_new ();
  g_ptr_array_add (ctx->games, game);

  gchar *title = title_for (ctx, item->gamename);
  GtkWidget *check = gtk_check_button_new ();
  gtk_widget_set_valign (check, GTK_ALIGN_CENTER);
  gchar *label = g_strdup_printf ("Download %s", title);
  gtk_accessible_update_property (GTK_ACCESSIBLE (check), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                  -1);
  g_free (label);
  g_signal_connect (check, "toggled", G_CALLBACK (on_check_toggled), ctx);
  game->check = GTK_CHECK_BUTTON (check);

  if (item->files->len == 0)
    {
      /* Saved before files were listed: nothing to open, so no arrow. */
      game->plain_row = ADW_ACTION_ROW (adw_action_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (game->plain_row), title);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (game->plain_row), FALSE);
      adw_action_row_add_prefix (game->plain_row, check);
      adw_action_row_set_activatable_widget (game->plain_row, check);
      refresh_game (game);
      gtk_list_box_append (GTK_LIST_BOX (list), GTK_WIDGET (game->plain_row));
      g_free (title);
      return;
    }

  game->row = ADW_EXPANDER_ROW (adw_expander_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (game->row), title);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (game->row), FALSE);
  adw_expander_row_add_prefix (game->row, check);

  add_language_row (game, title);
  for (guint p = 0; p < N_PARTS; p++)
    {
      AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), part_titles[p]);
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      gtk_widget_add_css_class (GTK_WIDGET (row), "gog-part-row");
      GtkWidget *part_check = gtk_check_button_new ();
      gtk_check_button_set_active (GTK_CHECK_BUTTON (part_check), TRUE);
      gtk_widget_set_valign (part_check, GTK_ALIGN_CENTER);
      gchar *part_label = g_strdup_printf ("Download the %s of %s",
                                           p == 0 ? "game files" : p == 1 ? "DLC" : "extras",
                                           title);
      gtk_accessible_update_property (GTK_ACCESSIBLE (part_check), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      part_label, -1);
      g_free (part_label);
      g_signal_connect (part_check, "toggled", G_CALLBACK (on_part_toggled), game);
      adw_action_row_add_prefix (row, part_check);
      adw_action_row_set_activatable_widget (row, part_check);
      game->part_rows[p] = row;
      game->part_checks[p] = GTK_CHECK_BUTTON (part_check);
      adw_expander_row_add_row (game->row, GTK_WIDGET (row));
      for (guint f = 0; f < item->files->len; f++)
        {
          GogSyncFile *file = g_ptr_array_index (item->files, f);
          if (file->part == parts[p])
            add_file_row (game, file);
        }
    }
  refresh_game (game);
  gtk_list_box_append (GTK_LIST_BOX (list), GTK_WIDGET (game->row));
  g_free (title);
}

typedef enum
{
  GROUP_NEW,
  GROUP_UPDATE,
  GROUP_OTHER_LANGUAGES,
} Group;

static Group
group_of (GogSyncItem *item)
{
  if (!gog_sync_item_has_wanted_files (item))
    return GROUP_OTHER_LANGUAGES;
  return item->kind == GOG_SYNC_NEW_GAME ? GROUP_NEW : GROUP_UPDATE;
}

static void
add_group (SyncCtx *ctx, Group group, const char *heading, const char *description)
{
  GtkWidget *list = NULL;
  for (guint i = 0; i < ctx->items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (ctx->items, i);
      if (group_of (item) != group)
        continue;
      if (!list)
        {
          GtkWidget *label = gtk_label_new (heading);
          gtk_widget_add_css_class (label, "heading");
          gtk_label_set_xalign (GTK_LABEL (label), 0);
          gtk_box_append (GTK_BOX (ctx->results_box), label);
          if (description)
            {
              GtkWidget *desc = gtk_label_new (description);
              gtk_label_set_wrap (GTK_LABEL (desc), TRUE);
              gtk_label_set_xalign (GTK_LABEL (desc), 0);
              gtk_widget_add_css_class (desc, "dim-label");
              gtk_box_append (GTK_BOX (ctx->results_box), desc);
            }
          list = gtk_list_box_new ();
          gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
          gtk_widget_add_css_class (list, "boxed-list");
          gtk_accessible_update_property (GTK_ACCESSIBLE (list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                          heading, -1);
          gtk_box_append (GTK_BOX (ctx->results_box), list);
        }
      add_game_row (ctx, list, item);
    }
}

/* Takes ownership of items. */
static void
show_items (SyncCtx *ctx, GPtrArray *items, gint64 checked_unix)
{
  /* The rows point at the old items. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child (ctx->results_box)))
    gtk_box_remove (GTK_BOX (ctx->results_box), child);
  g_ptr_array_set_size (ctx->games, 0);
  g_ptr_array_unref (ctx->items);
  ctx->items = items;

  guint n_new = 0, n_update = 0, n_other = 0;
  for (guint i = 0; i < items->len; i++)
    {
      Group group = group_of (g_ptr_array_index (items, i));
      if (group == GROUP_NEW)
        n_new++;
      else if (group == GROUP_UPDATE)
        n_update++;
      else
        n_other++;
    }
  add_group (ctx, GROUP_NEW, "Not Downloaded Yet", NULL);
  add_group (ctx, GROUP_UPDATE, "New Versions", NULL);
  add_group (ctx, GROUP_OTHER_LANGUAGES, "Only in Languages You Left Out",
             "These have new files only in languages you did not choose. Choose more "
             "languages to download them.");

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
  else if (n_new + n_update == 0)
    summary = g_strdup_printf ("Last checked %s: everything is downloaded and up to date.", when);
  else
    summary = g_strdup_printf ("Last checked %s: %u not downloaded yet, %u with a new version. "
                               "Choose what to download, and open a game to choose its "
                               "languages, DLC and extras. Old versions are deleted afterwards.",
                               when, n_new, n_update);
  gtk_label_set_text (ctx->summary_label, summary);
  gtk_widget_set_visible (ctx->results_scroll, items->len > 0);
  g_free (summary);
  g_free (when);
  (void) n_other;
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
hide_slots (SyncCtx *ctx)
{
  for (guint i = 0; i < N_SLOTS; i++)
    gtk_widget_set_visible (ctx->slots[i].box, FALSE);
  gtk_widget_set_visible (ctx->files_heading, FALSE);
}

static void
on_job_file_progress (GogSyncJob *job, gint slot, const gchar *filename, gdouble fraction,
                      const gchar *detail, gpointer user_data)
{
  (void) job;
  SyncCtx *ctx = user_data;
  if (slot < 0 || slot >= N_SLOTS)
    return;
  SlotView *view = &ctx->slots[slot];
  gtk_widget_set_visible (view->box, filename != NULL);
  if (filename)
    {
      gtk_label_set_text (view->name, filename);
      gtk_progress_bar_set_fraction (view->bar, fraction);
      gchar *text = detail ? g_strdup_printf ("%.0f%% — %s", fraction * 100, detail)
                           : g_strdup_printf ("%.0f%%", fraction * 100);
      gtk_progress_bar_set_text (view->bar, text);
      gtk_accessible_update_property (GTK_ACCESSIBLE (view->bar), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      filename, -1);
      g_free (text);
    }
  gboolean any = FALSE;
  for (guint i = 0; i < N_SLOTS; i++)
    any = any || gtk_widget_get_visible (ctx->slots[i].box);
  gtk_widget_set_visible (ctx->files_heading, any);
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
      gint64 checked = 0;
      g_ptr_array_unref (gog_sync_load_results (&checked)); /* a quick check keeps the time */
      show_items (ctx, items, checked);
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
on_failures_response (AdwAlertDialog *dialog, const char *response, gpointer user_data)
{
  (void) dialog;
  SyncCtx *ctx = user_data;
  GPtrArray *failures = g_object_get_data (G_OBJECT (dialog), "failures");
  if (g_strcmp0 (response, "ignore") == 0)
    {
      gog_sync_ignore_files (failures);
      gint64 checked = 0;
      GPtrArray *saved = gog_sync_load_results (&checked); /* sets checked first */
      show_items (ctx, saved, checked);
      set_busy (ctx, FALSE);
      append_log_line (ctx, failures->len == 1 ? "The file that failed is now ignored."
                                               : "The files that failed are now ignored.");
    }
  else
    append_log_line (ctx, "The files that failed stay in the list, to try again later.");
}

/* Lists the files that failed and why, and asks whether to keep them for
 * later or ignore them. */
static void
ask_about_failures (SyncCtx *ctx, GPtrArray *failures)
{
  GString *body = g_string_new (NULL);
  g_string_append (body, failures->len == 1 ? "This file could not be downloaded:\n\n"
                                            : "These files could not be downloaded:\n\n");
  for (guint i = 0; i < failures->len; i++)
    {
      GogSyncFailure *failure = g_ptr_array_index (failures, i);
      gchar *title = title_for (ctx, failure->gamename);
      g_string_append_printf (body, "• %s — %s: %s\n", title, failure->filename, failure->reason);
      g_free (title);
    }
  g_string_append (body, "\nTheir games keep the old version until they are downloaded. Try "
                         "again later, or ignore these files? Ignored files show up again when "
                         "GOG puts out a new version of them.");

  AdwDialog *dialog = adw_alert_dialog_new (failures->len == 1 ? "A File Could Not Be Downloaded"
                                                               : "Some Files Could Not Be Downloaded",
                                            body->str);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "ignore", "_Ignore",
                                  "later", "_Try Again Later", NULL);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "later");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "later");
  g_object_set_data_full (G_OBJECT (dialog), "failures", g_ptr_array_ref (failures),
                          (GDestroyNotify) g_ptr_array_unref);
  g_signal_connect (dialog, "response", G_CALLBACK (on_failures_response), ctx);
  /* No announce(): screen readers read an alert dialog as it opens, and
   * announcing while it opens crashed GTK 4.22. */
  adw_dialog_present (dialog, GTK_WIDGET (ctx->window));
  g_string_free (body, TRUE);
}

static void
on_download_finished (GogSyncJob *job, gboolean success, const gchar *error_message,
                      gpointer user_data)
{
  SyncCtx *ctx = user_data;
  /* Even a failed or cancelled run may have fetched some games. */
  ctx->downloaded = TRUE;
  GPtrArray *failures = gog_sync_job_get_failures (job);
  failures = failures && failures->len > 0 ? g_ptr_array_ref (failures) : NULL;
  if (success && !failures)
    {
      const char *msg = "Download finished. Old versions were deleted.";
      gtk_progress_bar_set_fraction (ctx->progress_bar, 1.0);
      gtk_progress_bar_set_text (ctx->progress_bar, "Download finished");
      append_log_line (ctx, msg);
      announce (ctx, msg);
    }
  else if (success)
    {
      gchar *msg = g_strdup_printf (failures->len == 1 ? "Download finished, but %u file failed."
                                                       : "Download finished, but %u files failed.",
                                    failures->len);
      gtk_progress_bar_set_text (ctx->progress_bar, msg);
      append_log_line (ctx, msg);
      g_free (msg);
    }
  else
    {
      gtk_progress_bar_set_text (ctx->progress_bar, "Download stopped");
      append_log_line (ctx, error_message ? error_message : "The download failed.");
      announce (ctx, error_message ? error_message : "The download failed.");
    }
  g_signal_handlers_disconnect_by_data (job, ctx);
  g_clear_object (&ctx->job);
  hide_slots (ctx);

  gint64 checked = 0;
  GPtrArray *saved = gog_sync_load_results (&checked); /* sets checked first */
  show_items (ctx, saved, checked);
  set_busy (ctx, FALSE);
  if (failures)
    {
      ask_about_failures (ctx, failures);
      g_ptr_array_unref (failures);
    }
}

static void
start_job (SyncCtx *ctx, GogSyncJob *job)
{
  ctx->job = job;
  g_signal_connect (job, "progress", G_CALLBACK (on_job_progress), ctx);
  g_signal_connect (job, "log-line", G_CALLBACK (on_job_log_line), ctx);
  g_signal_connect (job, "file-progress", G_CALLBACK (on_job_file_progress), ctx);
  hide_slots (ctx);
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (ctx->log_view), "", -1);
  gtk_widget_set_visible (ctx->log_scroll, FALSE); /* until there is something to show */
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

/* Results saved by an older version have no file lists, so their games
 * can't show their files. A check of just those games gets them. */
static void
fetch_missing_file_lists (SyncCtx *ctx)
{
  GStrvBuilder *builder = g_strv_builder_new ();
  guint n = 0;
  for (guint i = 0; i < ctx->items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (ctx->items, i);
      if (item->files->len == 0)
        {
          g_strv_builder_add (builder, item->gamename);
          n++;
        }
    }
  GStrv names = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  if (n > 0)
    {
      GogSyncJob *job = gog_sync_job_new_check_games (ctx->library_dir, (const char *const *) names);
      g_signal_connect (job, "finished", G_CALLBACK (on_check_finished), ctx);
      start_job (ctx, job);
      gtk_progress_bar_set_text (ctx->progress_bar, "Getting the file lists…");
    }
  g_strfreev (names);
}

static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  GArray *requests = g_array_new (FALSE, FALSE, sizeof (GogSyncRequest));
  for (guint i = 0; i < ctx->games->len; i++)
    {
      GameRow *game = g_ptr_array_index (ctx->games, i);
      GogSyncRequest r = { game->item->gamename, chosen_parts (game), game->languages };
      /* Results from before files were listed have no parts to choose. */
      if (game->item->files->len == 0)
        r.parts = GOG_SYNC_PART_ALL;
      if (gtk_check_button_get_active (game->check) && r.parts)
        g_array_append_val (requests, r);
    }
  if (requests->len > 0)
    {
      guint threads = (guint) adw_spin_row_get_value (ctx->threads_row);
      GogSyncJob *job = gog_sync_job_new_download (ctx->library_dir,
                                                   (const GogSyncRequest *) requests->data,
                                                   requests->len, threads);
      g_signal_connect (job, "finished", G_CALLBACK (on_download_finished), ctx);
      start_job (ctx, job);
    }
  g_array_unref (requests);
}

static void
on_select_all_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  for (guint i = 0; i < ctx->games->len; i++)
    gtk_check_button_set_active (((GameRow *) g_ptr_array_index (ctx->games, i))->check, TRUE);
}

static void
on_threads_changed (AdwSpinRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;
  gog_sync_set_threads ((guint) adw_spin_row_get_value (row));
}

static void
on_cancel_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  if (ctx->job)
    gog_sync_job_cancel (ctx->job);
}

static gboolean
on_close_request (GtkWindow *window, gpointer user_data)
{
  (void) window;
  SyncCtx *ctx = user_data;
  if (ctx->busy)
    {
      announce (ctx, "Still working. Press Cancel first to stop it.");
      return TRUE;
    }
  return FALSE;
}

static void
on_window_destroy (GtkWidget *window, gpointer user_data)
{
  (void) window;
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
  ctx->games = g_ptr_array_new_with_free_func ((GDestroyNotify) game_row_free);

  GtkWindow *window = GTK_WINDOW (adw_window_new ());
  ctx->window = window;
  gtk_window_set_title (window, "New Games and Updates on GOG");
  gtk_window_set_transient_for (window, parent);
  gtk_window_set_modal (window, TRUE);
  gtk_window_set_destroy_with_parent (window, TRUE);
  gtk_window_set_default_size (window, 640, 620);
  gtk_widget_set_size_request (GTK_WIDGET (window), 360, 320);

  /* Esc closes it, like the app's other dialogs. */
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();
  gtk_shortcut_controller_add_shortcut (
      GTK_SHORTCUT_CONTROLLER (shortcuts),
      gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                        gtk_named_action_new ("window.close")));
  gtk_widget_add_controller (GTK_WIDGET (window), shortcuts);

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
  /* The list gets all the room the window has. */
  gtk_widget_set_vexpand (ctx->results_scroll, TRUE);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (ctx->results_scroll), 120);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (ctx->results_scroll), ctx->results_box);
  gtk_box_append (GTK_BOX (outer), ctx->results_scroll);

  ctx->progress_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_visible (ctx->progress_box, FALSE);
  GtkWidget *total_heading = gtk_label_new ("Total");
  gtk_widget_add_css_class (total_heading, "heading");
  gtk_label_set_xalign (GTK_LABEL (total_heading), 0);
  gtk_box_append (GTK_BOX (ctx->progress_box), total_heading);
  ctx->progress_bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_progress_bar_set_show_text (ctx->progress_bar, TRUE);
  gtk_progress_bar_set_ellipsize (ctx->progress_bar, PANGO_ELLIPSIZE_MIDDLE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (ctx->progress_bar),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, "Total progress", -1);
  gtk_box_append (GTK_BOX (ctx->progress_box), GTK_WIDGET (ctx->progress_bar));

  ctx->files_heading = gtk_label_new ("Downloading Now");
  gtk_widget_add_css_class (ctx->files_heading, "heading");
  gtk_label_set_xalign (GTK_LABEL (ctx->files_heading), 0);
  gtk_widget_set_margin_top (ctx->files_heading, 4);
  gtk_box_append (GTK_BOX (ctx->progress_box), ctx->files_heading);
  for (guint i = 0; i < N_SLOTS; i++)
    {
      SlotView *view = &ctx->slots[i];
      view->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
      view->name = GTK_LABEL (gtk_label_new (NULL));
      gtk_label_set_xalign (view->name, 0);
      gtk_label_set_ellipsize (view->name, PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_add_css_class (GTK_WIDGET (view->name), "caption");
      view->bar = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
      gtk_progress_bar_set_show_text (view->bar, TRUE);
      gtk_progress_bar_set_ellipsize (view->bar, PANGO_ELLIPSIZE_END);
      gtk_box_append (GTK_BOX (view->box), GTK_WIDGET (view->name));
      gtk_box_append (GTK_BOX (view->box), GTK_WIDGET (view->bar));
      gtk_box_append (GTK_BOX (ctx->progress_box), view->box);
    }
  hide_slots (ctx);
  GtkWidget *log_scroll = gtk_scrolled_window_new ();
  ctx->log_scroll = log_scroll;
  gtk_widget_set_size_request (log_scroll, -1, 140);
  gtk_widget_set_visible (log_scroll, FALSE);
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

  GtkWidget *settings_list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (settings_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (settings_list, "boxed-list");
  ctx->threads_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, N_SLOTS, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->threads_row),
                                 "Downloads at the same time");
  adw_spin_row_set_value (ctx->threads_row, gog_sync_get_threads ());
  g_signal_connect (ctx->threads_row, "notify::value", G_CALLBACK (on_threads_changed), ctx);
  gtk_list_box_append (GTK_LIST_BOX (settings_list), GTK_WIDGET (ctx->threads_row));
  gtk_box_append (GTK_BOX (outer), settings_list);

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
  adw_window_set_content (ADW_WINDOW (window), toolbar_view);

  if (gog_sync_is_available ())
    {
      gint64 checked = 0;
      GPtrArray *saved = gog_sync_load_results (&checked); /* sets checked first */
      show_items (ctx, saved, checked);
      set_busy (ctx, FALSE);
      fetch_missing_file_lists (ctx);
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

  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), ctx);
  g_signal_connect (window, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (window);
}
