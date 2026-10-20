/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "main_window.h"
#include "game_detail.h"
#include "preferences_dialog.h"
#include "../library.h"
#include "../config.h"
#include "gog_sync_dialog.h"
#include "../gog_sync.h"
#include "../text_util.h"
#include "../categories.h"
#include <string.h>

struct _GogMainWindow
{
  AdwApplicationWindow parent_instance;

  GogLibrary *library;
  GogConfig *config;
  GogState *state;
  GHashTable *gog_updates; /* gamenames with a new version on GOG (last check) */
  GHashTable *dlc_counts;  /* GogGame* (base game) -> number of its DLC, as GUINT_TO_POINTER */

  GtkStack *root_stack; /* "empty" | "loading" | "browse" */
  AdwStatusPage *empty_page;
  GtkLabel *loading_label;
  GtkProgressBar *loading_progress;

  AdwOverlaySplitView *split_view;
  AdwNavigationView *nav_view;
  GogGameDetail *detail;
  GtkButton *back_button; /* header-bar "back to library" button; hidden on the library page */

  GtkFilterListModel *filter_model;
  GtkCustomFilter *filter;
  GtkSortListModel *sort_model;
  GtkCustomSorter *sorter;
  GtkSingleSelection *selection;
  GtkGridView *grid_view;
  GtkLabel *count_label;

  GtkSearchEntry *search_entry;
  GtkToggleButton *os_windows_btn;
  GtkToggleButton *os_linux_btn;
  GtkToggleButton *os_mac_btn;
  GtkToggleButton *content_dlc_btn;
  GtkToggleButton *content_extras_btn;
  GtkDropDown *language_dropdown;
  GStrv language_names;        /* GOG's names, for the dropdown's entries after "Any" */
  GtkDropDown *status_dropdown;
  GtkDropDown *category_dropdown;
  GStrv category_names;        /* the dropdown's entries after "Any category" */
  GtkLabel *category_status;   /* progress or failure of the category lookup */
  GtkDropDown *sort_dropdown;

  /* GTK fires "toggled"/"notify::selected" on some widgets as a side
   * effect of construction (e.g. GtkDropDown auto-selecting position 0
   * when given a model), which happens while build_sidebar() is still
   * running -- well before self->filter/self->filter_model exist. Guard
   * every filter/sort callback on this so those early, spurious signals
   * are no-ops instead of dereferencing not-yet-created objects. */
  gboolean ready;

  gboolean scanning;       /* a scan is in flight; don't start a second one */
  gboolean rescan_pending; /* start_scan() was called during a scan */
  GCancellable *scan_cancellable;

  gboolean categories_running; /* a category lookup is in flight */
  gboolean categories_pending; /* a scan finished during it; look again after */
  GCancellable *categories_cancellable;
};

G_DEFINE_FINAL_TYPE (GogMainWindow, gog_main_window, ADW_TYPE_APPLICATION_WINDOW)

typedef struct
{
  GogMainWindow *window;
  GtkPicture *cover;
  GtkLabel *title;
  GtkLabel *badges;
  GtkLabel *languages;
  GtkLabel *content;
} CardWidgets;

static void start_scan (GogMainWindow *self);
static void on_filters_changed (GtkWidget *widget, gpointer user_data);
static void on_sort_changed (GtkWidget *widget, gpointer user_data);

/* Whether the last GOG check found a new version of game. The library
 * folder of a game is named after its lgogdownloader game name. */
static gboolean
has_gog_update (GogMainWindow *self, GogGame *game)
{
  const char *folder = gog_game_get_folder_path (game);
  if (!self->gog_updates || !folder)
    return FALSE;
  gchar *name = g_path_get_basename (folder);
  gboolean found = g_hash_table_contains (self->gog_updates, name);
  g_free (name);
  return found;
}

static guint
dlc_count (GogMainWindow *self, GogGame *game)
{
  return self->dlc_counts ? GPOINTER_TO_UINT (g_hash_table_lookup (self->dlc_counts, game)) : 0;
}

/* After a scan: how many DLC each game has, for the DLC filter and cards. */
static void
rebuild_dlc_counts (GogMainWindow *self)
{
  if (self->dlc_counts)
    g_hash_table_remove_all (self->dlc_counts);
  else
    self->dlc_counts = g_hash_table_new (g_direct_hash, g_direct_equal);
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      GogGame *base = gog_game_get_base_game (g);
      if (base)
        g_hash_table_insert (self->dlc_counts, base,
                             GUINT_TO_POINTER (dlc_count (self, base) + 1));
      g_object_unref (g);
    }
}

/* Reloads the saved check results. Returns TRUE if the set changed. */
static gboolean
load_gog_updates (GogMainWindow *self)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *items = gog_sync_load_results (NULL);
  for (guint i = 0; i < items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (items, i);
      if (item->kind == GOG_SYNC_UPDATE && gog_sync_item_has_wanted_files (item))
        g_hash_table_add (set, g_strdup (item->gamename));
    }
  g_ptr_array_unref (items);

  gboolean changed = !self->gog_updates || g_hash_table_size (set) != g_hash_table_size (self->gog_updates);
  if (!changed)
    {
      GHashTableIter iter;
      gpointer key;
      g_hash_table_iter_init (&iter, set);
      while (!changed && g_hash_table_iter_next (&iter, &key, NULL))
        changed = !g_hash_table_contains (self->gog_updates, key);
    }
  if (self->gog_updates)
    g_hash_table_unref (self->gog_updates);
  self->gog_updates = set;
  return changed;
}

/* ---------------- filter / sort ---------------- */

static gboolean
filter_func (gpointer item, gpointer user_data)
{
  GogGame *game = GOG_GAME (item);
  GogMainWindow *self = user_data;

  /* DLC is listed on its game's page, not as a card of its own. */
  if (gog_game_get_base_game (game))
    return FALSE;

  const char *search = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  if (search && *search)
    {
      gchar *title_lower = g_utf8_strdown (gog_game_get_title (game), -1);
      gchar *search_lower = g_utf8_strdown (search, -1);
      gboolean match = strstr (title_lower, search_lower) != NULL;
      g_free (title_lower);
      g_free (search_lower);
      if (!match)
        return FALSE;
    }

  gboolean want_win = gtk_toggle_button_get_active (self->os_windows_btn);
  gboolean want_lin = gtk_toggle_button_get_active (self->os_linux_btn);
  gboolean want_mac = gtk_toggle_button_get_active (self->os_mac_btn);
  if (want_win || want_lin || want_mac)
    {
      gboolean ok = (want_win && gog_game_get_supports_os (game, GOG_OS_WINDOWS)) ||
                    (want_lin && gog_game_get_supports_os (game, GOG_OS_LINUX)) ||
                    (want_mac && gog_game_get_supports_os (game, GOG_OS_MAC));
      if (!ok)
        return FALSE;
    }

  gboolean want_dlc = gtk_toggle_button_get_active (self->content_dlc_btn);
  gboolean want_extras = gtk_toggle_button_get_active (self->content_extras_btn);
  if (want_dlc || want_extras)
    {
      gboolean ok = (want_dlc && dlc_count (self, game) > 0) ||
                    (want_extras && gog_game_get_extras (game)->len > 0);
      if (!ok)
        return FALSE;
    }

  guint lang_idx = gtk_drop_down_get_selected (self->language_dropdown);
  if (lang_idx != GTK_INVALID_LIST_POSITION && lang_idx != 0)
    {
      /* The dropdown shows labels; games carry GOG's names. */
      const char *wanted = self->language_names &&
                                   lang_idx - 1 < g_strv_length (self->language_names)
                               ? self->language_names[lang_idx - 1]
                               : NULL;
      gboolean found = FALSE;
      for (const char *const *l = gog_game_get_languages (game); l && *l; l++)
        if (g_strcmp0 (*l, wanted) == 0)
          {
            found = TRUE;
            break;
          }
      if (!found)
        {
          /* Also the languages of the installers on disk, which GOG's own
           * list occasionally leaves out. */
          GStrv downloaded = gog_game_dup_installer_languages (game);
          found = g_strv_contains ((const char *const *) downloaded, wanted);
          g_strfreev (downloaded);
        }
      if (!found)
        return FALSE;
    }

  guint cat_idx = gtk_drop_down_get_selected (self->category_dropdown);
  if (cat_idx != GTK_INVALID_LIST_POSITION && cat_idx != 0)
    {
      const char *wanted = self->category_names &&
                                   cat_idx - 1 < g_strv_length (self->category_names)
                               ? self->category_names[cat_idx - 1]
                               : NULL;
      const char *const *cats = gog_game_get_categories (game);
      if (!wanted || !cats || !g_strv_contains (cats, wanted))
        return FALSE;
    }

  guint status_idx = gtk_drop_down_get_selected (self->status_dropdown);
  if (status_idx == 1 && !gog_game_get_installed (game))
    return FALSE;
  if (status_idx == 2 && gog_game_get_installed (game))
    return FALSE;
  if (status_idx == 3 && !has_gog_update (self, game))
    return FALSE;

  return TRUE;
}

/* Games with the same set of downloaded installer languages sort together,
 * groups ordered by language name. Games without installers go last. */
static gint
compare_installer_languages (GogGame *ga, GogGame *gb)
{
  GStrv la = gog_game_dup_installer_languages (ga);
  GStrv lb = gog_game_dup_installer_languages (gb);
  gint result;
  if (!la[0] || !lb[0])
    result = (la[0] == NULL) - (lb[0] == NULL);
  else
    {
      gchar *ka = g_strjoinv ("\n", la);
      gchar *kb = g_strjoinv ("\n", lb);
      result = g_utf8_collate (ka, kb);
      g_free (ka);
      g_free (kb);
    }
  g_strfreev (la);
  g_strfreev (lb);
  return result;
}

/* By main category (GOG lists it first); games without any go last. */
static gint
compare_main_category (GogGame *ga, GogGame *gb)
{
  const char *const *ca = gog_game_get_categories (ga);
  const char *const *cb = gog_game_get_categories (gb);
  const char *a = ca ? ca[0] : NULL;
  const char *b = cb ? cb[0] : NULL;
  if (!a || !b)
    return (a == NULL) - (b == NULL);
  return g_utf8_collate (a, b);
}

static gint
sort_func (gconstpointer a, gconstpointer b, gpointer user_data)
{
  GogGame *ga = GOG_GAME ((gpointer) a);
  GogGame *gb = GOG_GAME ((gpointer) b);
  GogMainWindow *self = user_data;
  guint mode = gtk_drop_down_get_selected (self->sort_dropdown);

  switch (mode)
    {
    case 1: /* Title Z-A */
      return g_utf8_collate (gog_game_get_title (gb), gog_game_get_title (ga));
    case 2: /* Installed first */
      if (gog_game_get_installed (ga) != gog_game_get_installed (gb))
        return gog_game_get_installed (gb) - gog_game_get_installed (ga);
      return g_utf8_collate (gog_game_get_title (ga), gog_game_get_title (gb));
    case 3: /* Size, largest first */
      {
        guint64 sa = gog_game_get_total_size (ga);
        guint64 sb = gog_game_get_total_size (gb);
        if (sa == sb)
          return 0;
        return sa < sb ? 1 : -1;
      }
    case 4: /* Language of the downloaded installers */
      {
        gint by_lang = compare_installer_languages (ga, gb);
        if (by_lang != 0)
          return by_lang;
        return g_utf8_collate (gog_game_get_title (ga), gog_game_get_title (gb));
      }
    case 5: /* Category */
      {
        gint by_category = compare_main_category (ga, gb);
        if (by_category != 0)
          return by_category;
        return g_utf8_collate (gog_game_get_title (ga), gog_game_get_title (gb));
      }
    default: /* Title A-Z */
      return g_utf8_collate (gog_game_get_title (ga), gog_game_get_title (gb));
    }
}

static void
update_count_label (GogMainWindow *self)
{
  guint shown = g_list_model_get_n_items (G_LIST_MODEL (self->filter_model));
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  guint total = 0;
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      if (!gog_game_get_base_game (g))
        total++;
      g_object_unref (g);
    }
  gchar *text = g_strdup_printf ("%u / %u games", shown, total);
  gtk_label_set_label (self->count_label, text);
  g_free (text);
}

static void
on_filters_changed (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  GogMainWindow *self = user_data;
  if (!self->ready)
    return;
  gtk_filter_changed (GTK_FILTER (self->filter), GTK_FILTER_CHANGE_DIFFERENT);
  update_count_label (self);
}

static void
on_sort_changed (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  GogMainWindow *self = user_data;
  if (!self->ready)
    return;
  gtk_sorter_changed (GTK_SORTER (self->sorter), GTK_SORTER_CHANGE_DIFFERENT);
}

/* GtkDropDown reports selection via the "notify" signal, whose handler
 * signature is (object, pspec, user_data) -- one more argument than the
 * "toggled"/"search-changed" handlers above. These thin wrappers carry the
 * correct signature so user_data is genuinely 'self' and not the GParamSpec. */
/* Install/uninstall changed a game's status, which is both a filter and a
 * sort key. */
static void
on_detail_changed (GogGameDetail *detail, gpointer user_data)
{
  (void) detail;
  GogMainWindow *self = user_data;
  on_filters_changed (NULL, self);
  on_sort_changed (NULL, self);
}

static void
on_filter_dropdown_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) object;
  (void) pspec;
  on_filters_changed (NULL, user_data);
}

static void
on_sort_dropdown_changed (GObject *object, GParamSpec *pspec, gpointer user_data)
{
  (void) object;
  (void) pspec;
  on_sort_changed (NULL, user_data);
}

/* ---------------- grid card factory ---------------- */

static void
update_card (CardWidgets *cw, GogGame *game)
{
  gtk_label_set_label (cw->title, gog_game_get_title (game));

  GString *badges = g_string_new (NULL);
  if (gog_game_get_supports_os (game, GOG_OS_WINDOWS))
    g_string_append (badges, "Win ");
  if (gog_game_get_supports_os (game, GOG_OS_LINUX))
    g_string_append (badges, "Lin ");
  if (gog_game_get_supports_os (game, GOG_OS_MAC))
    g_string_append (badges, "Mac ");
  if (gog_game_get_installed (game))
    g_string_append (badges, "· Installed ");
  if (has_gog_update (cw->window, game))
    g_string_append (badges, "· Update on GOG");
  gtk_label_set_label (cw->badges, badges->str);
  g_string_free (badges, TRUE);

  /* Codes keep the card short; the full names go to tooltip and screen
   * readers. */
  GStrv names = gog_game_dup_installer_languages (game);
  GStrv codes = gog_game_dup_installer_language_codes (game);
  for (GStrv c = codes; *c; c++)
    {
      gchar *upper = g_utf8_strup (*c, -1);
      g_free (*c);
      *c = upper;
    }
  gchar *code_text = g_strjoinv (" · ", codes);
  gtk_label_set_label (cw->languages, code_text);
  gtk_widget_set_visible (GTK_WIDGET (cw->languages), codes[0] != NULL);
  if (names[0])
    {
      gchar *joined = g_strjoinv (", ", names);
      gchar *full = g_strdup_printf ("Installers in %s", joined);
      gtk_widget_set_tooltip_text (GTK_WIDGET (cw->languages), full);
      gtk_accessible_update_property (GTK_ACCESSIBLE (cw->languages),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, full, -1);
      g_free (full);
      g_free (joined);
    }
  g_free (code_text);
  g_strfreev (codes);
  g_strfreev (names);

  guint n_dlc = dlc_count (cw->window, game);
  guint n_extras = gog_game_get_extras (game)->len;
  GString *content = g_string_new (NULL);
  if (n_dlc > 0)
    g_string_append_printf (content, "%u DLC", n_dlc);
  if (n_extras > 0)
    g_string_append_printf (content, "%s%u %s", content->len ? " · " : "", n_extras,
                            n_extras == 1 ? "extra" : "extras");
  gtk_label_set_label (cw->content, content->str);
  gtk_widget_set_visible (GTK_WIDGET (cw->content), content->len > 0);
  g_string_free (content, TRUE);

  if (gog_game_get_icon_path (game))
    gtk_picture_set_filename (cw->cover, gog_game_get_icon_path (game));
  else
    gtk_picture_set_filename (cw->cover, NULL);
}

static void
on_game_installed_notify (GObject *game, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  GtkListItem *list_item = user_data;
  CardWidgets *cw = g_object_get_data (G_OBJECT (list_item), "cw");
  if (cw)
    update_card (cw, GOG_GAME (game));
}

static void
on_factory_setup (GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void) factory;

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (box, "gog-card");
  gtk_widget_set_size_request (box, 160, -1);

  GtkWidget *cover = gtk_picture_new ();
  gtk_widget_add_css_class (cover, "gog-card-cover");
  gtk_widget_set_size_request (cover, 152, 152);
  gtk_picture_set_content_fit (GTK_PICTURE (cover), GTK_CONTENT_FIT_COVER);
  gtk_box_append (GTK_BOX (box), cover);

  GtkWidget *title = gtk_label_new (NULL);
  gtk_widget_add_css_class (title, "gog-card-title");
  gtk_label_set_wrap (GTK_LABEL (title), TRUE);
  gtk_label_set_lines (GTK_LABEL (title), 2);
  gtk_label_set_ellipsize (GTK_LABEL (title), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign (GTK_LABEL (title), 0.0);
  gtk_box_append (GTK_BOX (box), title);

  GtkWidget *badges = gtk_label_new (NULL);
  gtk_widget_add_css_class (badges, "dim-label");
  gtk_widget_add_css_class (badges, "caption");
  gtk_label_set_xalign (GTK_LABEL (badges), 0.0);
  gtk_box_append (GTK_BOX (box), badges);

  GtkWidget *languages = gtk_label_new (NULL);
  gtk_widget_add_css_class (languages, "dim-label");
  gtk_widget_add_css_class (languages, "caption");
  gtk_label_set_wrap (GTK_LABEL (languages), TRUE);
  gtk_label_set_xalign (GTK_LABEL (languages), 0.0);
  gtk_box_append (GTK_BOX (box), languages);

  GtkWidget *content = gtk_label_new (NULL);
  gtk_widget_add_css_class (content, "dim-label");
  gtk_widget_add_css_class (content, "caption");
  gtk_label_set_wrap (GTK_LABEL (content), TRUE);
  gtk_label_set_xalign (GTK_LABEL (content), 0.0);
  gtk_box_append (GTK_BOX (box), content);

  gtk_list_item_set_child (list_item, box);

  CardWidgets *cw = g_new0 (CardWidgets, 1);
  cw->window = user_data;
  cw->cover = GTK_PICTURE (cover);
  cw->title = GTK_LABEL (title);
  cw->badges = GTK_LABEL (badges);
  cw->languages = GTK_LABEL (languages);
  cw->content = GTK_LABEL (content);
  g_object_set_data_full (G_OBJECT (list_item), "cw", cw, g_free);
}

static void
on_factory_bind (GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void) factory;
  (void) user_data;
  GogGame *game = gtk_list_item_get_item (list_item);
  CardWidgets *cw = g_object_get_data (G_OBJECT (list_item), "cw");
  update_card (cw, game);

  gulong handler = g_signal_connect (game, "notify::installed", G_CALLBACK (on_game_installed_notify),
                                      list_item);
  g_object_set_data (G_OBJECT (list_item), "notify-id", GSIZE_TO_POINTER (handler));
}

static void
on_factory_unbind (GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data)
{
  (void) factory;
  (void) user_data;
  GogGame *game = gtk_list_item_get_item (list_item);
  gulong handler = (gulong) GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (list_item), "notify-id"));
  if (game && handler)
    g_signal_handler_disconnect (game, handler);
}

static void
on_grid_activate (GtkGridView *grid_view, guint position, gpointer user_data)
{
  (void) grid_view;
  GogMainWindow *self = user_data;
  GogGame *game = g_list_model_get_item (G_LIST_MODEL (self->selection), position);
  if (!game)
    return;
  gog_game_detail_set_game (self->detail, game);
  adw_navigation_view_push_by_tag (self->nav_view, "detail");
  g_object_unref (game);
}

/* ---------------- scan lifecycle ---------------- */

static void
populate_language_dropdown (GogMainWindow *self)
{
  GtkStringList *list = gtk_string_list_new (NULL);
  gtk_string_list_append (list, "Any language");
  g_strfreev (self->language_names);
  self->language_names = gog_library_get_all_languages (self->library);
  for (GStrv l = self->language_names; l && *l; l++)
    {
      gchar *label = text_language_label (*l);
      gtk_string_list_append (list, label);
      g_free (label);
    }
  gtk_drop_down_set_model (self->language_dropdown, G_LIST_MODEL (list));
  gtk_drop_down_set_selected (self->language_dropdown, 0);
  g_object_unref (list);
}

/* ---------------- categories ---------------- */

static gint
collate_strings (gconstpointer a, gconstpointer b, gpointer user_data)
{
  (void) user_data;
  return g_utf8_collate (*(const char *const *) a, *(const char *const *) b);
}

/* Every category among the games with a card of their own, sorted. */
static void
populate_category_dropdown (GogMainWindow *self)
{
  /* Keep the chosen category across a rescan or a finished lookup. */
  guint old_idx = gtk_drop_down_get_selected (self->category_dropdown);
  gchar *old = self->category_names && old_idx != GTK_INVALID_LIST_POSITION && old_idx > 0 &&
                       old_idx - 1 < g_strv_length (self->category_names)
                   ? g_strdup (self->category_names[old_idx - 1])
                   : NULL;

  GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      if (!gog_game_get_base_game (g))
        for (const char *const *c = gog_game_get_categories (g); c && *c; c++)
          g_hash_table_add (seen, (gpointer) *c);
      g_object_unref (g);
    }
  /* The keys point into the games' own lists; copy them. */
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, seen);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (names, g_strdup (key));
  g_hash_table_unref (seen);
  g_ptr_array_sort_with_data (names, collate_strings, NULL);

  GtkStringList *list = gtk_string_list_new (NULL);
  gtk_string_list_append (list, "Any category");
  guint selected = 0;
  for (guint i = 0; i < names->len; i++)
    {
      const char *name = g_ptr_array_index (names, i);
      gtk_string_list_append (list, name);
      if (g_strcmp0 (name, old) == 0)
        selected = i + 1;
    }
  g_ptr_array_add (names, NULL);
  g_strfreev (self->category_names);
  self->category_names = (GStrv) g_ptr_array_free (names, FALSE);

  gtk_drop_down_set_model (self->category_dropdown, G_LIST_MODEL (list));
  gtk_drop_down_set_selected (self->category_dropdown, selected);
  g_object_unref (list);
  g_free (old);
}

/* Gives every game its categories from the cache. */
static void
apply_categories (GogMainWindow *self)
{
  GHashTable *cache = gog_categories_load ();
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, gog_game_get_id (g));
      GStrv cats = g_hash_table_lookup (cache, key);
      gog_game_set_categories (g, cats && cats[0] ? g_strdupv (cats) : NULL);
      g_free (key);
      g_object_unref (g);
    }
  g_hash_table_unref (cache);
  populate_category_dropdown (self);
}

static void
set_category_status (GogMainWindow *self, const char *text)
{
  gtk_label_set_label (self->category_status, text ? text : "");
  gtk_widget_set_visible (GTK_WIDGET (self->category_status), text != NULL);
}

typedef struct
{
  GogMainWindow *window;
  guint done;
  guint total;
} CategoryProgress;

static gboolean
category_progress_idle (gpointer data)
{
  CategoryProgress *p = data;
  if (p->window->categories_running && !gtk_widget_in_destruction (GTK_WIDGET (p->window)))
    {
      gchar *text = g_strdup_printf ("Fetching categories from GOG… %u of %u", p->done, p->total);
      set_category_status (p->window, text);
      g_free (text);
    }
  g_object_unref (p->window);
  g_free (p);
  return G_SOURCE_REMOVE;
}

/* On the lookup thread. */
static void
on_category_progress (guint done, guint total, gpointer user_data)
{
  CategoryProgress *p = g_new0 (CategoryProgress, 1);
  p->window = g_object_ref (user_data);
  p->done = done;
  p->total = total;
  g_idle_add (category_progress_idle, p);
}

static void
category_lookup_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GArray *ids = task_data;
  GError *error = NULL;
  guint looked_up = 0;
  if (gog_categories_lookup ((const gint64 *) (gpointer) ids->data, ids->len, cancellable,
                             on_category_progress, source, &looked_up, &error))
    g_task_return_int (task, looked_up);
  else
    g_task_return_error (task, error);
}

static void start_category_lookup (GogMainWindow *self);

static void
on_category_lookup_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  GogMainWindow *self = GOG_MAIN_WINDOW (source);
  self->categories_running = FALSE;
  g_clear_object (&self->categories_cancellable);
  GError *error = NULL;
  g_task_propagate_int (G_TASK (res), &error);
  if (gtk_widget_in_destruction (GTK_WIDGET (self)) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_clear_error (&error);
      return;
    }

  /* Whatever was found is in the cache, also after a failure. */
  apply_categories (self);
  on_filters_changed (NULL, self);
  on_sort_changed (NULL, self);
  if (error)
    {
      gchar *text = g_strdup_printf ("%s\nThe rest are fetched on the next rescan.",
                                     error->message);
      set_category_status (self, text);
      g_free (text);
      g_clear_error (&error);
    }
  else
    set_category_status (self, NULL);

  if (self->categories_pending)
    {
      self->categories_pending = FALSE;
      start_category_lookup (self);
    }
}

/* Fetches categories in the background for the games the cache doesn't
 * know yet; a no-op without network use when it knows them all. */
static void
start_category_lookup (GogMainWindow *self)
{
  if (self->categories_running)
    {
      self->categories_pending = TRUE;
      return;
    }
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  GListModel *model = gog_library_get_model (self->library);
  guint n = g_list_model_get_n_items (model);
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (model, i);
      gint64 id = gog_game_get_id (g);
      /* Only games with a card: DLC isn't sorted or filtered. */
      if (!gog_game_get_base_game (g) && id > 0)
        g_array_append_val (ids, id);
      g_object_unref (g);
    }
  if (ids->len == 0)
    {
      g_array_unref (ids);
      return;
    }

  self->categories_running = TRUE;
  self->categories_cancellable = g_cancellable_new ();
  GTask *task = g_task_new (self, self->categories_cancellable, on_category_lookup_done, NULL);
  g_task_set_task_data (task, ids, (GDestroyNotify) g_array_unref);
  g_task_run_in_thread (task, category_lookup_thread);
  g_object_unref (task);
}

static void
on_scan_progress (GogLibrary *library, gdouble fraction, const gchar *message, gpointer user_data)
{
  (void) library;
  GogMainWindow *self = user_data;
  if (fraction >= 0)
    gtk_progress_bar_set_fraction (self->loading_progress, fraction);
  gtk_label_set_label (self->loading_label, message);
}

#define EMPTY_PAGE_DESCRIPTION                                                                \
  "By default the current folder is scanned. "                                              \
  "Choose the folder containing your downloaded GOG games, "                                \
  "or set one permanently in Preferences."

static void
on_scan_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GogMainWindow *self = user_data; /* reffed by start_scan() */
  self->scanning = FALSE;
  g_clear_object (&self->scan_cancellable);

  if (gtk_widget_in_destruction (GTK_WIDGET (self)))
    {
      g_object_unref (self);
      return;
    }

  if (self->rescan_pending)
    {
      /* The library path changed (or Rescan was pressed) mid-scan; this
       * result is stale. */
      self->rescan_pending = FALSE;
      gog_library_scan_finish (self->library, res, NULL);
      start_scan (self);
      g_object_unref (self);
      return;
    }

  GError *error = NULL;
  if (!gog_library_scan_finish (self->library, res, &error))
    {
      /* Don't leave the spinner running forever: offer the folder picker,
       * with the reason. */
      gchar *desc = g_strdup_printf ("%s\n\n" EMPTY_PAGE_DESCRIPTION,
                                     error ? error->message : "Failed to scan library.");
      adw_status_page_set_title (self->empty_page, "Could Not Scan Library");
      adw_status_page_set_description (self->empty_page, desc);
      g_free (desc);
      g_clear_error (&error);
      gtk_stack_set_visible_child_name (self->root_stack, "empty");
      g_object_unref (self);
      return;
    }

  guint n = g_list_model_get_n_items (gog_library_get_model (self->library));
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (gog_library_get_model (self->library), i);
      gog_state_apply_to_game (self->state, g);
      g_object_unref (g);
    }
  rebuild_dlc_counts (self);
  on_filters_changed (NULL, self); /* the DLC filter needs the new counts */

  if (n == 0)
    {
      /* Nothing here — most likely the app was launched somewhere that isn't a
       * GOG games folder. Offer the folder picker rather than an empty list. */
      adw_status_page_set_title (self->empty_page, "No Games Found Here");
      adw_status_page_set_description (self->empty_page, EMPTY_PAGE_DESCRIPTION);
      gtk_stack_set_visible_child_name (self->root_stack, "empty");
      g_object_unref (self);
      return;
    }

  populate_language_dropdown (self);
  apply_categories (self);
  start_category_lookup (self);
  /* Re-filter/re-sort: install state was applied after the models saw the
   * new items. */
  on_filters_changed (NULL, self);
  on_sort_changed (NULL, self);
  gtk_stack_set_visible_child_name (self->root_stack, "browse");
  g_object_unref (self);
}

static void
start_scan (GogMainWindow *self)
{
  if (self->scanning)
    {
      self->rescan_pending = TRUE;
      return;
    }
  self->scanning = TRUE;
  /* The counts are keyed by the game objects this scan replaces. */
  if (self->dlc_counts)
    g_hash_table_remove_all (self->dlc_counts);

  /* The scan replaces every game object; don't leave a detail page open on
   * one that is about to be dropped from the library. */
  adw_navigation_view_pop_to_tag (self->nav_view, "library");

  gtk_stack_set_visible_child_name (self->root_stack, "loading");
  gtk_progress_bar_set_fraction (self->loading_progress, 0.0);
  gtk_label_set_label (self->loading_label, "Scanning library…");
  gchar *path = gog_config_effective_library_path (self->config);
  self->scan_cancellable = g_cancellable_new ();
  gog_library_scan_async (self->library, path, self->scan_cancellable, on_scan_done,
                          g_object_ref (self));
  g_free (path);
}

static void
on_choose_library_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GogMainWindow *self = user_data;
  GFile *folder = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
  if (!folder)
    return;

  g_free (self->config->library_path);
  self->config->library_path = g_file_get_path (folder);
  g_object_unref (folder);

  GError *error = NULL;
  if (!gog_config_save (self->config, &error))
    {
      g_warning ("Could not save config: %s", error->message);
      g_clear_error (&error);
    }

  start_scan (self);
}

static void
prompt_for_library (GogMainWindow *self)
{
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, "Choose Your GOG Games Directory");

  gchar *guess = gog_config_effective_library_path (self->config);
  if (g_file_test (guess, G_FILE_TEST_IS_DIR))
    {
      GFile *initial = g_file_new_for_path (guess);
      gtk_file_dialog_set_initial_folder (dialog, initial);
      g_object_unref (initial);
    }
  g_free (guess);

  gtk_file_dialog_select_folder (dialog, GTK_WINDOW (self), NULL, on_choose_library_response, self);
  g_object_unref (dialog);
}

static void
on_choose_library_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  prompt_for_library (user_data);
}

/* ---------------- header actions ---------------- */

static void
on_rescan_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  start_scan (self);
}

static void
on_library_path_changed_from_prefs (const char *new_path, gpointer user_data)
{
  (void) new_path;
  start_scan (user_data);
}

static void
on_preferences_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  gog_show_preferences_dialog (GTK_WINDOW (self), self->config, self->library, self->state,
                                on_library_path_changed_from_prefs, self);
}

static void
on_gog_sync_done (gboolean downloaded, gpointer user_data)
{
  GogMainWindow *self = user_data;
  /* The updates window is destroyed with this one; don't rescan then. */
  if (gtk_widget_in_destruction (GTK_WIDGET (self)))
    return;
  gboolean changed = load_gog_updates (self);
  if (downloaded || changed)
    start_scan (self); /* new files, or new "Update on GOG" marks on the cards */
}

static void
on_gog_sync_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  gchar *library_dir = gog_config_effective_library_path (self->config);
  gog_show_sync_dialog (GTK_WINDOW (self), library_dir, self->library, on_gog_sync_done, self);
  g_free (library_dir);
}

static void
on_about_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  /* AdwAboutWindow, not AdwAboutDialog: a real window can be moved and
   * resized like the app's other dialogs. libadwaita marks it deprecated,
   * but it is still the only about box that is a window of its own. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  GtkWindow *about = g_object_new (ADW_TYPE_ABOUT_WINDOW, "transient-for", GTK_WINDOW (self),
                                   "modal", TRUE, "application-name", "GOG Library Manager",
                                   "application-icon", "org.gogmanager.app",
                                   "version", GOG_MANAGER_VERSION, "developer-name", "Hypnotize",
                                   "comments",
                                   "Browse, sort, and install your local GOG offline installers "
                                   "— Windows titles via Wine or Proton, Linux titles natively.",
                                   "license-type", GTK_LICENSE_GPL_3_0, NULL);
  G_GNUC_END_IGNORE_DEPRECATIONS
  gtk_window_present (about);
}

static void
on_sidebar_toggle_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  gboolean shown = adw_overlay_split_view_get_show_sidebar (self->split_view);
  adw_overlay_split_view_set_show_sidebar (self->split_view, !shown);
}

static void
on_back_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  adw_navigation_view_pop (self->nav_view);
}

static void
on_nav_visible_page_changed (GObject *nav_view, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  GogMainWindow *self = user_data;
  AdwNavigationPage *page = adw_navigation_view_get_visible_page (ADW_NAVIGATION_VIEW (nav_view));
  const char *tag = page ? adw_navigation_page_get_tag (page) : NULL;
  gtk_widget_set_visible (GTK_WIDGET (self->back_button), g_strcmp0 (tag, "library") != 0);
}

/* ---------------- construction ---------------- */

/* Icon-only controls need a name for screen readers; a tooltip alone is
 * only a description. */
static void
set_accessible_label (GtkWidget *widget, const char *label)
{
  gtk_accessible_update_property (GTK_ACCESSIBLE (widget), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static GtkWidget *
build_sidebar (GogMainWindow *self)
{
  GtkWidget *scroller = gtk_scrolled_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_add_css_class (box, "gog-sidebar");
  gtk_widget_set_size_request (box, 220, -1);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box);

  self->count_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->count_label), "dim-label");
  gtk_widget_set_halign (GTK_WIDGET (self->count_label), GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->count_label));

  GtkWidget *os_heading = gtk_label_new ("Operating System");
  gtk_widget_add_css_class (os_heading, "heading");
  gtk_widget_set_halign (os_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), os_heading);

  GtkWidget *os_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (os_box, "linked");
  self->os_windows_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Windows"));
  self->os_linux_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Linux"));
  self->os_mac_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("macOS"));
  gtk_box_append (GTK_BOX (os_box), GTK_WIDGET (self->os_windows_btn));
  gtk_box_append (GTK_BOX (os_box), GTK_WIDGET (self->os_linux_btn));
  gtk_box_append (GTK_BOX (os_box), GTK_WIDGET (self->os_mac_btn));
  gtk_box_append (GTK_BOX (box), os_box);
  g_signal_connect (self->os_windows_btn, "toggled", G_CALLBACK (on_filters_changed), self);
  g_signal_connect (self->os_linux_btn, "toggled", G_CALLBACK (on_filters_changed), self);
  g_signal_connect (self->os_mac_btn, "toggled", G_CALLBACK (on_filters_changed), self);

  GtkWidget *content_heading = gtk_label_new ("Content");
  gtk_widget_add_css_class (content_heading, "heading");
  gtk_widget_set_halign (content_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), content_heading);

  /* Either or both: with both on, games with DLC or extras are shown. */
  GtkWidget *content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (content_box, "linked");
  self->content_dlc_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("DLC"));
  self->content_extras_btn = GTK_TOGGLE_BUTTON (gtk_toggle_button_new_with_label ("Extras"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->content_dlc_btn), "Only games that have DLC");
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->content_extras_btn),
                               "Only games that have extras, such as manuals and soundtracks");
  set_accessible_label (GTK_WIDGET (self->content_dlc_btn), "Only games that have DLC");
  set_accessible_label (GTK_WIDGET (self->content_extras_btn), "Only games that have extras");
  gtk_box_append (GTK_BOX (content_box), GTK_WIDGET (self->content_dlc_btn));
  gtk_box_append (GTK_BOX (content_box), GTK_WIDGET (self->content_extras_btn));
  gtk_box_append (GTK_BOX (box), content_box);
  g_signal_connect (self->content_dlc_btn, "toggled", G_CALLBACK (on_filters_changed), self);
  g_signal_connect (self->content_extras_btn, "toggled", G_CALLBACK (on_filters_changed), self);

  GtkWidget *lang_heading = gtk_label_new ("Language");
  gtk_widget_add_css_class (lang_heading, "heading");
  gtk_widget_set_halign (lang_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), lang_heading);
  self->language_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
  set_accessible_label (GTK_WIDGET (self->language_dropdown), "Language");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->language_dropdown));
  g_signal_connect (self->language_dropdown, "notify::selected", G_CALLBACK (on_filter_dropdown_changed), self);

  GtkWidget *category_heading = gtk_label_new ("Category");
  gtk_widget_add_css_class (category_heading, "heading");
  gtk_widget_set_halign (category_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), category_heading);
  self->category_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
  set_accessible_label (GTK_WIDGET (self->category_dropdown), "Category");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->category_dropdown));
  g_signal_connect (self->category_dropdown, "notify::selected", G_CALLBACK (on_filter_dropdown_changed), self);
  self->category_status = GTK_LABEL (gtk_label_new (NULL));
  gtk_label_set_wrap (self->category_status, TRUE);
  gtk_label_set_xalign (self->category_status, 0);
  gtk_widget_add_css_class (GTK_WIDGET (self->category_status), "dim-label");
  gtk_widget_add_css_class (GTK_WIDGET (self->category_status), "caption");
  gtk_widget_set_visible (GTK_WIDGET (self->category_status), FALSE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->category_status));

  GtkWidget *status_heading = gtk_label_new ("Status");
  gtk_widget_add_css_class (status_heading, "heading");
  gtk_widget_set_halign (status_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), status_heading);
  GtkStringList *status_list = gtk_string_list_new ((const char *[]) { "All games", "Installed",
                                                                        "Not installed",
                                                                        "New version on GOG", NULL });
  self->status_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (status_list), NULL));
  set_accessible_label (GTK_WIDGET (self->status_dropdown), "Status");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->status_dropdown));
  g_signal_connect (self->status_dropdown, "notify::selected", G_CALLBACK (on_filter_dropdown_changed), self);

  GtkWidget *sort_heading = gtk_label_new ("Sort By");
  gtk_widget_add_css_class (sort_heading, "heading");
  gtk_widget_set_halign (sort_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), sort_heading);
  GtkStringList *sort_list = gtk_string_list_new (
      (const char *[]) { "Title A–Z", "Title Z–A", "Installed first", "Size (largest)",
                         "Language", "Category", NULL });
  self->sort_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (sort_list), NULL));
  set_accessible_label (GTK_WIDGET (self->sort_dropdown), "Sort By");
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->sort_dropdown));
  g_signal_connect (self->sort_dropdown, "notify::selected", G_CALLBACK (on_sort_dropdown_changed), self);

  return scroller;
}

static GtkWidget *
build_header_bar (GogMainWindow *self)
{
  GtkWidget *header = adw_header_bar_new ();

  self->back_button = GTK_BUTTON (gtk_button_new_from_icon_name ("go-previous-symbolic"));
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->back_button), "Back to Library");
  set_accessible_label (GTK_WIDGET (self->back_button), "Back to Library");
  gtk_widget_set_visible (GTK_WIDGET (self->back_button), FALSE);
  g_signal_connect (self->back_button, "clicked", G_CALLBACK (on_back_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (self->back_button));

  GtkWidget *sidebar_toggle = gtk_button_new_from_icon_name ("sidebar-show-symbolic");
  gtk_widget_set_tooltip_text (sidebar_toggle, "Show or Hide Filters");
  set_accessible_label (sidebar_toggle, "Show or Hide Filters");
  g_signal_connect (sidebar_toggle, "clicked", G_CALLBACK (on_sidebar_toggle_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), sidebar_toggle);

  self->search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->search_entry), 260, -1);
  gtk_search_entry_set_placeholder_text (self->search_entry, "Search your library…");
  set_accessible_label (GTK_WIDGET (self->search_entry), "Search your library");
  g_signal_connect (self->search_entry, "search-changed", G_CALLBACK (on_filters_changed), self);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->search_entry));

  GtkWidget *rescan_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_tooltip_text (rescan_btn, "Rescan Library");
  set_accessible_label (rescan_btn, "Rescan Library");
  g_signal_connect (rescan_btn, "clicked", G_CALLBACK (on_rescan_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), rescan_btn);

  GtkWidget *sync_btn = gtk_button_new_from_icon_name ("software-update-available-symbolic");
  gtk_widget_set_tooltip_text (sync_btn, "New Games and Updates on GOG");
  set_accessible_label (sync_btn, "New Games and Updates on GOG");
  g_signal_connect (sync_btn, "clicked", G_CALLBACK (on_gog_sync_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), sync_btn);

  GtkWidget *about_btn = gtk_button_new_from_icon_name ("help-about-symbolic");
  gtk_widget_set_tooltip_text (about_btn, "About");
  set_accessible_label (about_btn, "About");
  g_signal_connect (about_btn, "clicked", G_CALLBACK (on_about_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), about_btn);

  GtkWidget *prefs_btn = gtk_button_new_from_icon_name ("preferences-system-symbolic");
  gtk_widget_set_tooltip_text (prefs_btn, "Preferences");
  set_accessible_label (prefs_btn, "Preferences");
  g_signal_connect (prefs_btn, "clicked", G_CALLBACK (on_preferences_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), prefs_btn);

  return header;
}

static void
gog_main_window_dispose (GObject *object)
{
  GogMainWindow *self = GOG_MAIN_WINDOW (object);
  if (self->scan_cancellable)
    g_cancellable_cancel (self->scan_cancellable);
  if (self->categories_cancellable)
    g_cancellable_cancel (self->categories_cancellable);
  if (self->library)
    g_signal_handlers_disconnect_by_data (self->library, self);
  g_clear_pointer (&self->gog_updates, g_hash_table_unref);
  g_clear_pointer (&self->dlc_counts, g_hash_table_unref);
  G_OBJECT_CLASS (gog_main_window_parent_class)->dispose (object);
}

static void
gog_main_window_finalize (GObject *object)
{
  GogMainWindow *self = GOG_MAIN_WINDOW (object);
  g_clear_object (&self->library);
  g_strfreev (self->language_names);
  g_strfreev (self->category_names);
  gog_config_free (self->config);
  gog_state_free (self->state);
  G_OBJECT_CLASS (gog_main_window_parent_class)->finalize (object);
}

static void
gog_main_window_class_init (GogMainWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = gog_main_window_dispose;
  object_class->finalize = gog_main_window_finalize;
}

static void
gog_main_window_init (GogMainWindow *self)
{
  (void) self;
}

GogMainWindow *
gog_main_window_new (GtkApplication *app)
{
  GogMainWindow *self = g_object_new (GOG_TYPE_MAIN_WINDOW, "application", app, NULL);

  self->library = gog_library_new ();
  self->config = gog_config_load ();
  self->state = gog_state_load ();
  load_gog_updates (self);
  g_signal_connect (self->library, "scan-progress", G_CALLBACK (on_scan_progress), self);

  gtk_window_set_title (GTK_WINDOW (self), "GOG Library Manager");
  gtk_window_set_default_size (GTK_WINDOW (self), 1200, 800);

  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), build_header_bar (self));

  self->root_stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->root_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (self->root_stack));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toolbar_view);

  /* --- empty page (no library path chosen yet) --- */
  GtkWidget *empty_page = adw_status_page_new ();
  self->empty_page = ADW_STATUS_PAGE (empty_page);
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty_page), "folder-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Games Found Here");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page), EMPTY_PAGE_DESCRIPTION);
  GtkWidget *choose_btn = gtk_button_new_with_label ("Choose Folder…");
  gtk_widget_add_css_class (choose_btn, "suggested-action");
  gtk_widget_add_css_class (choose_btn, "pill");
  gtk_widget_set_halign (choose_btn, GTK_ALIGN_CENTER);
  g_signal_connect (choose_btn, "clicked", G_CALLBACK (on_choose_library_clicked), self);
  adw_status_page_set_child (ADW_STATUS_PAGE (empty_page), choose_btn);
  gtk_stack_add_named (self->root_stack, empty_page, "empty");

  /* --- loading page --- */
  GtkWidget *loading_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign (loading_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign (loading_box, GTK_ALIGN_CENTER);
  GtkWidget *spinner = adw_spinner_new ();
  gtk_widget_set_size_request (spinner, 48, 48);
  gtk_box_append (GTK_BOX (loading_box), spinner);
  self->loading_label = GTK_LABEL (gtk_label_new ("Scanning library…"));
  gtk_box_append (GTK_BOX (loading_box), GTK_WIDGET (self->loading_label));
  self->loading_progress = GTK_PROGRESS_BAR (gtk_progress_bar_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->loading_progress), 260, -1);
  gtk_box_append (GTK_BOX (loading_box), GTK_WIDGET (self->loading_progress));
  gtk_stack_add_named (self->root_stack, loading_box, "loading");

  /* --- browse page: sidebar + nav view (library grid / detail) --- */
  self->split_view = ADW_OVERLAY_SPLIT_VIEW (adw_overlay_split_view_new ());
  adw_overlay_split_view_set_sidebar (self->split_view, build_sidebar (self));
  adw_overlay_split_view_set_min_sidebar_width (self->split_view, 200);
  adw_overlay_split_view_set_max_sidebar_width (self->split_view, 280);

  self->nav_view = ADW_NAVIGATION_VIEW (adw_navigation_view_new ());
  g_signal_connect (self->nav_view, "notify::visible-page", G_CALLBACK (on_nav_visible_page_changed), self);

  /* library grid page */
  self->filter = gtk_custom_filter_new (filter_func, self, NULL);
  self->filter_model =
      gtk_filter_list_model_new (gog_library_get_model (self->library), GTK_FILTER (self->filter));
  self->sorter = gtk_custom_sorter_new (sort_func, self, NULL);
  self->sort_model = gtk_sort_list_model_new (G_LIST_MODEL (self->filter_model), GTK_SORTER (self->sorter));
  self->selection = gtk_single_selection_new (G_LIST_MODEL (self->sort_model));
  gtk_single_selection_set_autoselect (self->selection, FALSE);
  gtk_single_selection_set_can_unselect (self->selection, TRUE);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (on_factory_setup), self);
  g_signal_connect (factory, "bind", G_CALLBACK (on_factory_bind), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (on_factory_unbind), self);

  self->grid_view = GTK_GRID_VIEW (gtk_grid_view_new (GTK_SELECTION_MODEL (self->selection), factory));
  g_object_set (self->grid_view, "single-click-activate", TRUE, NULL);
  g_signal_connect (self->grid_view, "activate", G_CALLBACK (on_grid_activate), self);

  GtkWidget *grid_scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (grid_scroller), GTK_WIDGET (self->grid_view));

  AdwNavigationPage *library_page = adw_navigation_page_new_with_tag (grid_scroller, "Library", "library");
  adw_navigation_view_add (self->nav_view, library_page);

  /* detail page */
  self->detail = gog_game_detail_new (GTK_WINDOW (self), self->library, self->config, self->state);
  g_signal_connect (self->detail, GOG_GAME_DETAIL_SIGNAL_CHANGED, G_CALLBACK (on_detail_changed), self);
  AdwNavigationPage *detail_page =
      adw_navigation_page_new_with_tag (GTK_WIDGET (self->detail), "Details", "detail");
  adw_navigation_view_add (self->nav_view, detail_page);

  adw_overlay_split_view_set_content (self->split_view, GTK_WIDGET (self->nav_view));
  gtk_stack_add_named (self->root_stack, GTK_WIDGET (self->split_view), "browse");

  self->ready = TRUE;

  /* Always scan: the effective path defaults to the current working directory
   * when the user hasn't set a library folder in Preferences. */
  start_scan (self);

  return self;
}
