/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gog_sync_dialog.h"
#include "../gog_sync.h"
#include "dialog_window.h"
#include "../installer_engine.h"
#include "../text_util.h"
#include "login_dialog.h"
#include <string.h>

#define N_SLOTS 8 /* the most downloads at the same time */
#define N_PARTS 4

/* Patches are only listed when the download settings include them. */
static const GogSyncPart parts[N_PARTS] = { GOG_SYNC_PART_GAME, GOG_SYNC_PART_DLC,
                                            GOG_SYNC_PART_EXTRA, GOG_SYNC_PART_PATCH };
static const char *const part_titles[N_PARTS] = { "Game", "DLC", "Extras", "Patches" };
static const char *const part_names[N_PARTS] = { "game files", "DLC", "extras", "patches" };

typedef struct _SyncCtx SyncCtx;

/* One listed game: whether to download it, which of its parts, and in
 * which languages. */
typedef struct
{
  SyncCtx *ctx;
  GogSyncItem *item;
  guint languages; /* what gets downloaded, as far as offered; 0 = all */
  guint choice;    /* GOG_SYNC_LANGUAGES_DEFAULT, 0 = all, or the chosen */
  AdwExpanderRow *row; /* NULL when the files are not known; then plain_row */
  AdwActionRow *plain_row;
  GtkCheckButton *check;
  AdwActionRow *part_rows[N_PARTS];
  GtkCheckButton *part_checks[N_PARTS];
  AdwActionRow *language_row;
  GtkCheckButton *default_languages;
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
  AdwExpanderRow *settings_row; /* folds the three below; its subtitle sums them up */
  AdwSpinRow *threads_row;
  AdwSpinRow *rate_row;
  AdwSwitchRow *hidden_row;
  AdwSwitchRow *patches_row;
  GtkWidget *log_scroll;
  GtkTextView *log_view;
  GtkButton *check_button;
  GtkButton *quick_button;
  GtkWidget *login_button; /* in the header bar */
  int logged_in;            /* -1 not known yet, else whether lgogdownloader is */
  GCancellable *login_check; /* the check when the window opens */
  GtkButton *select_all_button;
  GtkButton *download_button;
  GtkButton *cancel_button;

  GPtrArray *items; /* GogSyncItem*, what is listed */
  GPtrArray *games; /* GameRow*, in display order */

  GogSyncJob *job;
  gboolean busy;
  gboolean downloaded;
  gchar *focus_gamename; /* the game to tick and open; NULL for none */
};

static void
ctx_free (SyncCtx *ctx)
{
  if (ctx->login_check)
    g_cancellable_cancel (ctx->login_check);
  g_clear_object (&ctx->login_check);
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
  g_free (ctx->focus_gamename);
  g_free (ctx);
}

static void
announce (SyncCtx *ctx, const char *message)
{
  gog_announce (GTK_WIDGET (ctx->window), message);
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
  GogGame *known = gog_library_find_by_gamename (ctx->library, gamename);
  if (known)
    return g_strdup (gog_game_get_title (known));
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

/* "Log In to GOG…" while lgogdownloader isn't logged in (or it isn't known
 * yet), greyed "Logged In to GOG" once it is. */
static void
update_login_button (SyncCtx *ctx)
{
  gboolean in = ctx->logged_in == 1;
  gtk_button_set_label (GTK_BUTTON (ctx->login_button), in ? "Logged In to GOG" : "Log In to GOG…");
  gtk_widget_set_tooltip_text (ctx->login_button,
                               in ? "lgogdownloader is logged in to your GOG account"
                                  : "Log lgogdownloader in to your GOG account");
  gtk_widget_set_sensitive (ctx->login_button, !ctx->busy && !in && gog_sync_is_available ());
}

static void
set_login_state (SyncCtx *ctx, gboolean logged_in)
{
  ctx->logged_in = logged_in;
  update_login_button (ctx);
}

static void
set_busy (SyncCtx *ctx, gboolean busy)
{
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->check_button), !busy);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->quick_button), !busy);
  update_login_button (ctx);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->select_all_button), !busy && ctx->items->len > 0);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), !busy && any_checked (ctx));
  gtk_widget_set_sensitive (ctx->results_box, !busy);
  gtk_widget_set_sensitive (GTK_WIDGET (ctx->settings_row), !busy);
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), busy);
  /* Closing mid-run would orphan the job; use Cancel instead (see
   * on_close_request). */
  ctx->busy = busy;
  update_login_button (ctx);
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

/* The library's game with this gamename when its folder has another name
 * (lgogdownloader then sees no folder and would download a second copy),
 * else NULL. Transfer none. */
static GogGame *
misnamed_game (SyncCtx *ctx, const char *gamename)
{
  GogGame *game = gog_library_find_by_gamename (ctx->library, gamename);
  return game && gog_library_folder_misnamed (game, ctx->library_dir) ? game : NULL;
}

/* subtitle, with a note first when the game's folder has another name.
 * Takes subtitle. */
static gchar *
with_folder_note (GameRow *game, gchar *subtitle)
{
  GogGame *misnamed = misnamed_game (game->ctx, game->item->gamename);
  if (!misnamed)
    return subtitle;
  gchar *folder = g_path_get_basename (gog_game_get_folder_path (misnamed));
  gchar *out = g_strdup_printf ("In your library in the folder \u201c%s\u201d · %s", folder,
                                subtitle);
  g_free (folder);
  g_free (subtitle);
  return out;
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
      subtitle = with_folder_note (game, subtitle);
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
  subtitle = with_folder_note (game, subtitle);
  adw_expander_row_set_subtitle (game->row, subtitle);
  g_free (subtitle);
  g_string_free (summary, TRUE);

  if (game->language_row)
    {
      gchar *langs;
      if (game->choice == GOG_SYNC_LANGUAGES_DEFAULT)
        {
          gchar *def = gog_sync_describe_default_for (game->item->gamename,
                                                      game->item->languages_offered);
          langs = g_strdup_printf ("Default: %s", def);
          g_free (def);
        }
      else
        langs = gog_sync_describe_languages (game->languages);
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
  if (check == game->default_languages || check == game->all_languages)
    {
      /* "Default" and "All languages" clear the others; neither can be
       * turned off on its own. */
      gtk_check_button_set_active (check, TRUE);
      gtk_check_button_set_active (check == game->default_languages ? game->all_languages
                                                                    : game->default_languages,
                                   FALSE);
      for (guint i = 0; i < game->language_checks->len; i++)
        gtk_check_button_set_active (g_ptr_array_index (game->language_checks, i), FALSE);
      game->choice = check == game->default_languages ? GOG_SYNC_LANGUAGES_DEFAULT : 0;
    }
  else
    {
      guint chosen = 0;
      for (guint i = 0; i < game->language_checks->len; i++)
        {
          GtkCheckButton *c = g_ptr_array_index (game->language_checks, i);
          if (gtk_check_button_get_active (c))
            chosen |= GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (c), "bit"));
        }
      /* Unticking the last language goes back to the default. */
      game->choice = chosen ? chosen : GOG_SYNC_LANGUAGES_DEFAULT;
      gtk_check_button_set_active (game->default_languages, chosen == 0);
      gtk_check_button_set_active (game->all_languages, FALSE);
    }
  game->updating = FALSE;
  gog_sync_set_game_languages (game->item->gamename, game->choice);
  game->languages = gog_sync_item_languages (game->item);
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
  /* The subtitle can name Wikidata's developers: text, not markup. */
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (game->language_row), FALSE);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start (box, 6);
  gtk_widget_set_margin_end (box, 6);
  gtk_widget_set_margin_top (box, 6);
  gtk_widget_set_margin_bottom (box, 6);
  gboolean by_default = game->choice == GOG_SYNC_LANGUAGES_DEFAULT;
  game->default_languages = language_check (game, box, "Default (set in Preferences)", by_default);
  game->all_languages = language_check (game, box, "All languages", game->choice == 0);
  for (guint i = 0; i < n_langs; i++)
    if (offered & (1u << i))
      {
        GtkCheckButton *c = language_check (game, box, langs[i].name,
                                            !by_default && (game->choice & (1u << i)) != 0);
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
  game->choice = gog_sync_get_game_languages (item->gamename);
  game->language_checks = g_ptr_array_new ();
  game->file_rows = g_ptr_array_new ();
  g_ptr_array_add (ctx->games, game);

  gchar *title = item->title ? g_strdup (item->title) : title_for (ctx, item->gamename);
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
                                           part_names[p],
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

  /* GOG's notes on the version it has now, folded away: they can be long. */
  gchar *changelog = item->changelog ? text_changelog_markup (item->changelog) : NULL;
  if (changelog && *changelog)
    {
      GtkWidget *notes = adw_expander_row_new ();
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (notes), "Changelog");
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (notes), FALSE);
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (notes),
                                     item->kind == GOG_SYNC_UPDATE
                                         ? "What changed, up to the version on GOG"
                                         : "GOG's notes on each version");
      gtk_widget_add_css_class (notes, "gog-part-row");
      GtkWidget *label = gtk_label_new (NULL);
      gtk_label_set_markup (GTK_LABEL (label), changelog);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_selectable (GTK_LABEL (label), TRUE);
      gtk_widget_set_margin_start (label, 12);
      gtk_widget_set_margin_end (label, 12);
      gtk_widget_set_margin_top (label, 8);
      gtk_widget_set_margin_bottom (label, 8);
      adw_expander_row_add_row (ADW_EXPANDER_ROW (notes), label);
      adw_expander_row_add_row (game->row, notes);
    }
  g_free (changelog);
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
  /* GOG's own news from that check, for the forum, chat and friends
   * aren't anywhere else in the app. */
  gchar *notes = when ? gog_sync_get_notifications () : NULL;
  if (notes)
    {
      gchar *with = g_strdup_printf ("%s\nOn GOG: %s.", summary, notes);
      g_free (summary);
      summary = with;
    }
  g_free (notes);
  gtk_label_set_text (ctx->summary_label, summary);
  gtk_widget_set_visible (ctx->results_scroll, items->len > 0);
  g_free (summary);
  g_free (when);
  (void) n_other;

  /* Opened from a game's Download button: that game, ready to go. */
  for (guint i = 0; ctx->focus_gamename && i < ctx->games->len; i++)
    {
      GameRow *game = g_ptr_array_index (ctx->games, i);
      if (g_strcmp0 (game->item->gamename, ctx->focus_gamename) != 0)
        continue;
      gtk_check_button_set_active (game->check, TRUE);
      if (game->row)
        adw_expander_row_set_expanded (game->row, TRUE);
      /* Also before the window is shown. */
      gtk_window_set_focus (ctx->window, GTK_WIDGET (game->check));
      break;
    }
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
  gboolean needs_login = !success && gog_sync_job_needs_login (job);
  g_signal_handlers_disconnect_by_data (job, ctx);
  g_clear_object (&ctx->job);
  /* A check gets past the login only when lgogdownloader is logged in. */
  if (success || needs_login)
    ctx->logged_in = success;
  set_busy (ctx, FALSE);
  /* What to do about it is one button away. */
  if (needs_login)
    gtk_widget_grab_focus (ctx->login_button);
}

typedef struct
{
  SyncCtx *ctx;
  GPtrArray *failures; /* GogSyncFailure*, reffed */
} FailuresAsk;

static void
failures_ask_free (FailuresAsk *ask)
{
  g_ptr_array_unref (ask->failures);
  g_free (ask);
}

static void
on_failures_response (const char *response, gpointer user_data)
{
  FailuresAsk *ask = user_data;
  SyncCtx *ctx = ask->ctx;
  GPtrArray *failures = ask->failures;
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

  GogAlert *alert = gog_alert_new (failures->len == 1 ? "A File Could Not Be Downloaded"
                                                       : "Some Files Could Not Be Downloaded",
                                   body->str);
  gog_alert_add_response (alert, "ignore", "_Ignore", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "later", "_Try Again Later", ADW_RESPONSE_SUGGESTED);
  gog_alert_set_default_response (alert, "later");
  gog_alert_set_close_response (alert, "later");
  FailuresAsk *ask = g_new0 (FailuresAsk, 1);
  ask->ctx = ctx;
  ask->failures = g_ptr_array_ref (failures);
  /* Freed with the alert, also when it goes with the sync window. */
  gog_alert_set_user_data_free (alert, (GDestroyNotify) failures_ask_free);
  /* No announce(): screen readers read a new window's description as it
   * opens, and announcing while an alert opened crashed GTK 4.22. */
  gog_alert_present (alert, ctx->window, on_failures_response, ask);
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

/* The login window may outlive this one (it doesn't, being modal over it,
 * but its callback comes from its "destroy"), so it gets a weak ref. */
static void
on_logged_in (gboolean logged_in, gpointer user_data)
{
  GWeakRef *ref = user_data;
  GtkWindow *window = g_weak_ref_get (ref);
  g_weak_ref_clear (ref);
  g_free (ref);
  SyncCtx *ctx = window ? g_object_get_data (G_OBJECT (window), "sync-ctx") : NULL;
  if (ctx && logged_in)
    set_login_state (ctx, TRUE);
  if (window)
    g_object_unref (window);
}

static void
on_login_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  GWeakRef *ref = g_new0 (GWeakRef, 1);
  g_weak_ref_init (ref, ctx->window);
  gog_show_login_dialog (ctx->window, on_logged_in, ref);
}

static void
on_login_status (GObject *source, GAsyncResult *res, gpointer user_data)
{
  gchar *out = NULL;
  GError *error = NULL;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &out, NULL, &error);
  /* Cancelled: the window is gone, and user_data with it. */
  if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      SyncCtx *ctx = user_data;
      if (out && strstr (out, "Login status: Logged in"))
        set_login_state (ctx, TRUE);
      else if (out && strstr (out, "Not logged in"))
        set_login_state (ctx, FALSE);
    }
  g_clear_error (&error);
  g_free (out);
  g_object_unref (source);
}

/* Whether lgogdownloader is logged in, for the header bar's button. */
static void
check_login_state (SyncCtx *ctx)
{
  const char *args[] = { "--check-login-status", NULL };
  gchar **argv = gog_sync_lgog_argv (args);
  GSubprocess *proc = g_subprocess_newv ((const gchar *const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                             G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         NULL);
  g_free (argv);
  if (!proc)
    return;
  ctx->login_check = g_cancellable_new ();
  g_subprocess_communicate_utf8_async (proc, NULL, ctx->login_check, on_login_status, ctx);
}

static void
on_quick_check_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  GogSyncJob *job = gog_sync_job_new_quick_check (ctx->library_dir);
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

/* What is ticked, and in *bytes (may be NULL) the size of the files that
 * will come, as far as it is known. */
static GArray *
chosen_requests (SyncCtx *ctx, guint64 *bytes)
{
  GArray *requests = g_array_new (FALSE, FALSE, sizeof (GogSyncRequest));
  guint64 total = 0;
  for (guint i = 0; i < ctx->games->len; i++)
    {
      GameRow *game = g_ptr_array_index (ctx->games, i);
      GogSyncRequest r = { game->item->gamename, chosen_parts (game), game->languages };
      /* Results from before files were listed have no parts to choose. */
      if (game->item->files->len == 0)
        r.parts = GOG_SYNC_PART_ALL;
      /* Switched off since the check: they won't be downloaded. */
      if (!gog_sync_get_include_patches ())
        r.parts &= ~GOG_SYNC_PART_PATCH;
      if (!gtk_check_button_get_active (game->check) || !r.parts)
        continue;
      g_array_append_val (requests, r);
      for (guint f = 0; f < game->item->files->len; f++)
        {
          GogSyncFile *file = g_ptr_array_index (game->item->files, f);
          if ((file->part & r.parts) && gog_sync_file_wanted (file, r.languages))
            total += file->size;
        }
    }
  if (bytes)
    *bytes = total;
  return requests;
}

static void
run_download (SyncCtx *ctx)
{
  GArray *requests = chosen_requests (ctx, NULL);
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

/* The chosen games whose library folder has another name than GOG's. */
static GPtrArray *
chosen_misnamed (SyncCtx *ctx)
{
  GPtrArray *games = g_ptr_array_new ();
  GArray *requests = chosen_requests (ctx, NULL);
  for (guint i = 0; i < requests->len; i++)
    {
      GogGame *game = misnamed_game (ctx, g_array_index (requests, GogSyncRequest, i).gamename);
      if (game)
        g_ptr_array_add (games, game);
    }
  g_array_unref (requests);
  return games;
}

static void
on_misnamed_response (const char *response, gpointer user_data)
{
  SyncCtx *ctx = user_data;
  if (ctx->job || g_strcmp0 (response, "cancel") == 0)
    return;
  if (g_strcmp0 (response, "rename") == 0)
    {
      GPtrArray *games = chosen_misnamed (ctx);
      for (guint i = 0; i < games->len; i++)
        {
          GogGame *game = g_ptr_array_index (games, i);
          gchar *old = g_path_get_basename (gog_game_get_folder_path (game));
          GError *error = NULL;
          gchar *line;
          if (gog_library_rename_folder (ctx->library, game, &error))
            line = g_strdup_printf ("Renamed the folder \u201c%s\u201d to \u201c%s\u201d.", old,
                                    gog_game_get_gamename (game));
          else
            {
              line = g_strdup_printf ("Could not rename the folder \u201c%s\u201d: %s.", old,
                                      error->message);
              g_clear_error (&error);
            }
          append_log_line (ctx, line);
          g_free (line);
          g_free (old);
        }
      g_ptr_array_unref (games);
      for (guint i = 0; i < ctx->games->len; i++)
        refresh_game (g_ptr_array_index (ctx->games, i));
    }
  run_download (ctx);
}

/* lgogdownloader keeps each game in a folder named as on GOG. A game whose
 * folder was renamed looks missing to it, so it would download all of it
 * again into a new folder: offer to rename the folder back first. */
static void
start_download (SyncCtx *ctx)
{
  GPtrArray *games = chosen_misnamed (ctx);
  if (games->len == 0)
    {
      g_ptr_array_unref (games);
      run_download (ctx);
      return;
    }
  GString *body = g_string_new (games->len == 1
                                    ? "This game is in a folder with another name than GOG's. "
                                    : "These games are in folders with other names than GOG's. ");
  g_string_append (body, "lgogdownloader only looks for a game in a folder named as on GOG, so "
                         "it would download all of it again as a second copy.\n\n");
  for (guint i = 0; i < games->len; i++)
    {
      GogGame *game = g_ptr_array_index (games, i);
      gchar *folder = g_path_get_basename (gog_game_get_folder_path (game));
      g_string_append_printf (body, "\u2022 %s: \u201c%s\u201d \u2192 \u201c%s\u201d\n",
                              gog_game_get_title (game), folder, gog_game_get_gamename (game));
      g_free (folder);
    }
  g_string_append (body, "\nRenaming changes nothing else: installed games keep working.");
  GogAlert *alert = gog_alert_new (games->len == 1 ? "Rename the Game's Folder?"
                                                   : "Rename the Games' Folders?",
                                   body->str);
  gog_alert_add_response (alert, "cancel", "_Cancel", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "copy", "Download a _Second Copy", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "rename", "_Rename and Download", ADW_RESPONSE_SUGGESTED);
  gog_alert_set_default_response (alert, "rename");
  gog_alert_set_close_response (alert, "cancel");
  gog_alert_present (alert, ctx->window, on_misnamed_response, ctx);
  g_string_free (body, TRUE);
  g_ptr_array_unref (games);
}

static void
on_low_space_response (const char *response, gpointer user_data)
{
  SyncCtx *ctx = user_data;
  if (g_strcmp0 (response, "download") == 0 && !ctx->job)
    start_download (ctx);
}

/* Downloads, after a warning when the library's drive can't hold what is
 * coming. Our own check, not lgogdownloader's --check-free-space: that one
 * adds up every file of the games, also those already on disk, so an
 * update to a big game would be refused. New files arrive before the old
 * versions are deleted, so all of it has to fit. */
static void
on_download_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  guint64 needed = 0;
  g_array_unref (chosen_requests (ctx, &needed));
  guint64 free_bytes = 0;
  if (needed == 0 || !gog_free_space (ctx->library_dir, &free_bytes) || free_bytes >= needed)
    {
      start_download (ctx);
      return;
    }

  gchar *free_text = g_format_size (free_bytes);
  gchar *needed_text = g_format_size (needed);
  gchar *body = g_strdup_printf ("The downloads need %s, but only %s is free on the drive for "
                                 "%s. Downloads that don't fit will fail.",
                                 needed_text, free_text, ctx->library_dir);
  GogAlert *alert = gog_alert_new ("Not Enough Free Space", body);
  gog_alert_add_response (alert, "cancel", "_Cancel", ADW_RESPONSE_DEFAULT);
  gog_alert_add_response (alert, "download", "_Download Anyway", ADW_RESPONSE_DESTRUCTIVE);
  gog_alert_set_default_response (alert, "cancel");
  gog_alert_set_close_response (alert, "cancel");
  gog_alert_present (alert, ctx->window, on_low_space_response, ctx);
  g_free (body);
  g_free (needed_text);
  g_free (free_text);
}

static void
on_select_all_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  SyncCtx *ctx = user_data;
  for (guint i = 0; i < ctx->games->len; i++)
    gtk_check_button_set_active (((GameRow *) g_ptr_array_index (ctx->games, i))->check, TRUE);
}

/* "3 at a time · No speed limit · Hidden games too" */
static void
update_settings_summary (SyncCtx *ctx)
{
  guint threads = (guint) adw_spin_row_get_value (ctx->threads_row);
  guint rate = (guint) adw_spin_row_get_value (ctx->rate_row);
  GString *text = g_string_new (NULL);
  g_string_append_printf (text, "%u at a time · ", threads);
  if (rate > 0)
    g_string_append_printf (text, "At most %u kB/s", rate);
  else
    g_string_append (text, "No speed limit");
  if (adw_switch_row_get_active (ctx->hidden_row))
    g_string_append (text, " · Hidden games too");
  if (adw_switch_row_get_active (ctx->patches_row))
    g_string_append (text, " · Patches too");
  adw_expander_row_set_subtitle (ctx->settings_row, text->str);
  g_string_free (text, TRUE);
}

static void
on_threads_changed (AdwSpinRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  gog_sync_set_threads ((guint) adw_spin_row_get_value (row));
  update_settings_summary (user_data);
}

static void
on_rate_changed (AdwSpinRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  gog_sync_set_rate_limit ((guint) adw_spin_row_get_value (row));
  update_settings_summary (user_data);
}

static void
on_hidden_changed (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  gog_sync_set_include_hidden (adw_switch_row_get_active (row));
  update_settings_summary (user_data);
}

static void
on_patches_changed (AdwSwitchRow *row, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  gog_sync_set_include_patches (adw_switch_row_get_active (row));
  update_settings_summary (user_data);
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
  SyncCtx *ctx = user_data;
  g_object_set_data (G_OBJECT (window), "sync-ctx", NULL);
  if (ctx->on_done)
    ctx->on_done (ctx->downloaded, ctx->user_data);
  ctx_free (ctx);
}

static void show_dialog (GtkWindow *parent, const char *library_dir, GogLibrary *library,
                         const char *focus_gamename, GogSyncDoneFunc on_done,
                         gpointer user_data);

void
gog_show_sync_dialog (GtkWindow *parent, const char *library_dir, GogLibrary *library,
                      GogSyncDoneFunc on_done, gpointer user_data)
{
  show_dialog (parent, library_dir, library, NULL, on_done, user_data);
}

void
gog_show_sync_dialog_for_game (GtkWindow *parent, const char *library_dir, GogLibrary *library,
                               const char *gamename, GogSyncDoneFunc on_done, gpointer user_data)
{
  show_dialog (parent, library_dir, library, gamename, on_done, user_data);
}

static void
show_dialog (GtkWindow *parent, const char *library_dir, GogLibrary *library,
             const char *focus_gamename, GogSyncDoneFunc on_done, gpointer user_data)
{
  SyncCtx *ctx = g_new0 (SyncCtx, 1);
  ctx->focus_gamename = g_strdup (focus_gamename);
  ctx->logged_in = -1;
  ctx->library_dir = g_strdup (library_dir);
  ctx->library = library;
  ctx->on_done = on_done;
  ctx->user_data = user_data;
  ctx->items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);
  ctx->games = g_ptr_array_new_with_free_func ((GDestroyNotify) game_row_free);

  GtkWindow *window = GTK_WINDOW (adw_window_new ());
  ctx->window = window;
  /* For the login window's callback, which only has the window. */
  g_object_set_data (G_OBJECT (window), "sync-ctx", ctx);
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
  GtkWidget *header = adw_header_bar_new ();
  ctx->login_button = gtk_button_new_with_label ("Log In to GOG…");
  gtk_widget_set_tooltip_text (ctx->login_button,
                               "Log lgogdownloader in to your GOG account, in your web browser");
  g_signal_connect (ctx->login_button, "clicked", G_CALLBACK (on_login_clicked), ctx);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), ctx->login_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

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
  ctx->settings_row = ADW_EXPANDER_ROW (adw_expander_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->settings_row), "Download Settings");
  ctx->threads_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (1, N_SLOTS, 1));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->threads_row),
                                 "Downloads at the same time");
  adw_spin_row_set_value (ctx->threads_row, gog_sync_get_threads ());
  adw_expander_row_add_row (ctx->settings_row, GTK_WIDGET (ctx->threads_row));

  ctx->rate_row = ADW_SPIN_ROW (adw_spin_row_new_with_range (0, 1000000, 100));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->rate_row), "Speed limit (kB/s)");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->rate_row),
                               "For all downloads together; 0 for no limit");
  adw_spin_row_set_value (ctx->rate_row, gog_sync_get_rate_limit ());
  adw_expander_row_add_row (ctx->settings_row, GTK_WIDGET (ctx->rate_row));

  ctx->hidden_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->hidden_row),
                                 "Include games hidden on GOG");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->hidden_row),
                               "The ones you hid on your GOG account page; from the next check");
  adw_switch_row_set_active (ctx->hidden_row, gog_sync_get_include_hidden ());
  adw_expander_row_add_row (ctx->settings_row, GTK_WIDGET (ctx->hidden_row));

  ctx->patches_row = ADW_SWITCH_ROW (adw_switch_row_new ());
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (ctx->patches_row), "Include patches");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (ctx->patches_row),
                               "GOG's patches for older versions. Rarely needed: the installers "
                               "are always the newest version. From the next check");
  adw_switch_row_set_active (ctx->patches_row, gog_sync_get_include_patches ());
  adw_expander_row_add_row (ctx->settings_row, GTK_WIDGET (ctx->patches_row));

  update_settings_summary (ctx);
  g_signal_connect (ctx->threads_row, "notify::value", G_CALLBACK (on_threads_changed), ctx);
  g_signal_connect (ctx->rate_row, "notify::value", G_CALLBACK (on_rate_changed), ctx);
  g_signal_connect (ctx->hidden_row, "notify::active", G_CALLBACK (on_hidden_changed), ctx);
  g_signal_connect (ctx->patches_row, "notify::active", G_CALLBACK (on_patches_changed), ctx);
  gtk_list_box_append (GTK_LIST_BOX (settings_list), GTK_WIDGET (ctx->settings_row));
  gtk_box_append (GTK_BOX (outer), settings_list);

  GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
  ctx->cancel_button = GTK_BUTTON (gtk_button_new_with_label ("Cancel"));
  gtk_widget_set_visible (GTK_WIDGET (ctx->cancel_button), FALSE);
  g_signal_connect (ctx->cancel_button, "clicked", G_CALLBACK (on_cancel_clicked), ctx);
  ctx->select_all_button = GTK_BUTTON (gtk_button_new_with_label ("Select All"));
  g_signal_connect (ctx->select_all_button, "clicked", G_CALLBACK (on_select_all_clicked), ctx);
  ctx->quick_button = GTK_BUTTON (gtk_button_new_with_label ("Quick Check"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (ctx->quick_button),
                               "Check only the games GOG marks as updated or new: much faster "
                               "than Check Now, which looks at every game");
  g_signal_connect (ctx->quick_button, "clicked", G_CALLBACK (on_quick_check_clicked), ctx);
  ctx->check_button = GTK_BUTTON (gtk_button_new_with_label ("Check Now"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (ctx->check_button),
                               "Check every game you own (takes a few minutes)");
  g_signal_connect (ctx->check_button, "clicked", G_CALLBACK (on_check_clicked), ctx);
  ctx->download_button = GTK_BUTTON (gtk_button_new_with_label ("Download Selected"));
  gtk_widget_add_css_class (GTK_WIDGET (ctx->download_button), "suggested-action");
  g_signal_connect (ctx->download_button, "clicked", G_CALLBACK (on_download_clicked), ctx);
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->cancel_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->select_all_button));
  gtk_box_append (GTK_BOX (btn_box), GTK_WIDGET (ctx->quick_button));
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
      check_login_state (ctx);
      fetch_missing_file_lists (ctx);
    }
  else
    {
      gtk_label_set_text (ctx->summary_label,
                          "This needs lgogdownloader, which was not found. The GOG Manager "
                          "AppImage comes with it; otherwise install it from your "
                          "distribution's packages (see the README) and open this again.");
      gtk_widget_set_visible (ctx->results_scroll, FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->check_button), FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->quick_button), FALSE);
      gtk_widget_set_sensitive (ctx->login_button, FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->select_all_button), FALSE);
      gtk_widget_set_sensitive (GTK_WIDGET (ctx->download_button), FALSE);
    }

  g_signal_connect (window, "close-request", G_CALLBACK (on_close_request), ctx);
  g_signal_connect (window, "destroy", G_CALLBACK (on_window_destroy), ctx);
  gtk_window_present (window);
}
