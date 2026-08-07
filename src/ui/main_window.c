/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "main_window.h"
#include "game_detail.h"
#include "preferences_dialog.h"
#include "../library.h"
#include "../config.h"
#include <string.h>

struct _GogMainWindow
{
  AdwApplicationWindow parent_instance;

  GogLibrary *library;
  GogConfig *config;
  GogState *state;

  GtkStack *root_stack; /* "empty" | "loading" | "browse" */
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
  GtkDropDown *language_dropdown;
  GtkDropDown *status_dropdown;
  GtkDropDown *sort_dropdown;

  /* GTK fires "toggled"/"notify::selected" on some widgets as a side
   * effect of construction (e.g. GtkDropDown auto-selecting position 0
   * when given a model), which happens while build_sidebar() is still
   * running -- well before self->filter/self->filter_model exist. Guard
   * every filter/sort callback on this so those early, spurious signals
   * are no-ops instead of dereferencing not-yet-created objects. */
  gboolean ready;
};

G_DEFINE_FINAL_TYPE (GogMainWindow, gog_main_window, ADW_TYPE_APPLICATION_WINDOW)

typedef struct
{
  GtkPicture *cover;
  GtkLabel *title;
  GtkLabel *badges;
} CardWidgets;

static void start_scan (GogMainWindow *self);

/* ---------------- filter / sort ---------------- */

static gboolean
filter_func (gpointer item, gpointer user_data)
{
  GogGame *game = GOG_GAME (item);
  GogMainWindow *self = user_data;

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

  guint lang_idx = gtk_drop_down_get_selected (self->language_dropdown);
  if (lang_idx != GTK_INVALID_LIST_POSITION && lang_idx != 0)
    {
      GtkStringList *list = GTK_STRING_LIST (gtk_drop_down_get_model (self->language_dropdown));
      const char *wanted = gtk_string_list_get_string (list, lang_idx);
      gboolean found = FALSE;
      for (const char *const *l = gog_game_get_languages (game); l && *l; l++)
        if (g_strcmp0 (*l, wanted) == 0)
          {
            found = TRUE;
            break;
          }
      if (!found)
        return FALSE;
    }

  guint status_idx = gtk_drop_down_get_selected (self->status_dropdown);
  if (status_idx == 1 && !gog_game_get_installed (game))
    return FALSE;
  if (status_idx == 2 && gog_game_get_installed (game))
    return FALSE;

  return TRUE;
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
    default: /* Title A-Z */
      return g_utf8_collate (gog_game_get_title (ga), gog_game_get_title (gb));
    }
}

static void
update_count_label (GogMainWindow *self)
{
  guint shown = g_list_model_get_n_items (G_LIST_MODEL (self->filter_model));
  guint total = g_list_model_get_n_items (gog_library_get_model (self->library));
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
    g_string_append (badges, "· Installed");
  gtk_label_set_label (cw->badges, badges->str);
  g_string_free (badges, TRUE);

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
  (void) user_data;

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

  gtk_list_item_set_child (list_item, box);

  CardWidgets *cw = g_new0 (CardWidgets, 1);
  cw->cover = GTK_PICTURE (cover);
  cw->title = GTK_LABEL (title);
  cw->badges = GTK_LABEL (badges);
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
  GStrv langs = gog_library_get_all_languages (self->library);
  for (GStrv l = langs; l && *l; l++)
    gtk_string_list_append (list, *l);
  g_strfreev (langs);
  gtk_drop_down_set_model (self->language_dropdown, G_LIST_MODEL (list));
  gtk_drop_down_set_selected (self->language_dropdown, 0);
  g_object_unref (list);
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

static void
on_scan_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GogMainWindow *self = user_data;
  GError *error = NULL;
  if (!gog_library_scan_finish (self->library, res, &error))
    {
      gtk_label_set_label (self->loading_label,
                            error ? error->message : "Failed to scan library.");
      g_clear_error (&error);
      return;
    }

  guint n = g_list_model_get_n_items (gog_library_get_model (self->library));
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (gog_library_get_model (self->library), i);
      g_object_unref (g);
      gog_state_apply_to_game (self->state, g);
    }

  if (n == 0)
    {
      /* Nothing here — most likely the app was launched somewhere that isn't a
       * GOG games folder. Offer the folder picker rather than an empty list. */
      gtk_stack_set_visible_child_name (self->root_stack, "empty");
      return;
    }

  populate_language_dropdown (self);
  update_count_label (self);
  gtk_stack_set_visible_child_name (self->root_stack, "browse");
}

static void
start_scan (GogMainWindow *self)
{
  gtk_stack_set_visible_child_name (self->root_stack, "loading");
  gtk_progress_bar_set_fraction (self->loading_progress, 0.0);
  gtk_label_set_label (self->loading_label, "Scanning library…");
  gchar *path = gog_config_effective_library_path (self->config);
  gog_library_scan_async (self->library, path, NULL, on_scan_done, self);
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

  const char *guess = "/run/media/hypnotize/Games/GOG/Games";
  if (g_file_test (guess, G_FILE_TEST_IS_DIR))
    {
      GFile *initial = g_file_new_for_path (guess);
      gtk_file_dialog_set_initial_folder (dialog, initial);
      g_object_unref (initial);
    }

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
  gog_show_preferences_dialog (GTK_WINDOW (self), self->config, on_library_path_changed_from_prefs,
                                self);
}

static void
on_about_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  GogMainWindow *self = user_data;
  AdwDialog *about = g_object_new (ADW_TYPE_ABOUT_DIALOG, "application-name", "GOG Library Manager",
                                    "version", "0.1.0", "developer-name", "You",
                                    "comments",
                                    "Browse, sort, and install your local GOG offline installers "
                                    "— Windows titles via Wine or Proton, Linux titles natively.",
                                    "license-type", GTK_LICENSE_GPL_3_0, NULL);
  adw_dialog_present (about, GTK_WIDGET (self));
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

  GtkWidget *lang_heading = gtk_label_new ("Language");
  gtk_widget_add_css_class (lang_heading, "heading");
  gtk_widget_set_halign (lang_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), lang_heading);
  self->language_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (NULL, NULL));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->language_dropdown));
  g_signal_connect (self->language_dropdown, "notify::selected", G_CALLBACK (on_filter_dropdown_changed), self);

  GtkWidget *status_heading = gtk_label_new ("Status");
  gtk_widget_add_css_class (status_heading, "heading");
  gtk_widget_set_halign (status_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), status_heading);
  GtkStringList *status_list = gtk_string_list_new ((const char *[]) { "All games", "Installed",
                                                                        "Not installed", NULL });
  self->status_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (status_list), NULL));
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (self->status_dropdown));
  g_signal_connect (self->status_dropdown, "notify::selected", G_CALLBACK (on_filter_dropdown_changed), self);

  GtkWidget *sort_heading = gtk_label_new ("Sort By");
  gtk_widget_add_css_class (sort_heading, "heading");
  gtk_widget_set_halign (sort_heading, GTK_ALIGN_START);
  gtk_box_append (GTK_BOX (box), sort_heading);
  GtkStringList *sort_list = gtk_string_list_new (
      (const char *[]) { "Title A–Z", "Title Z–A", "Installed first", "Size (largest)", NULL });
  self->sort_dropdown = GTK_DROP_DOWN (gtk_drop_down_new (G_LIST_MODEL (sort_list), NULL));
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
  gtk_widget_set_visible (GTK_WIDGET (self->back_button), FALSE);
  g_signal_connect (self->back_button, "clicked", G_CALLBACK (on_back_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), GTK_WIDGET (self->back_button));

  GtkWidget *sidebar_toggle = gtk_button_new_from_icon_name ("sidebar-show-symbolic");
  g_signal_connect (sidebar_toggle, "clicked", G_CALLBACK (on_sidebar_toggle_clicked), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), sidebar_toggle);

  self->search_entry = GTK_SEARCH_ENTRY (gtk_search_entry_new ());
  gtk_widget_set_size_request (GTK_WIDGET (self->search_entry), 260, -1);
  gtk_search_entry_set_placeholder_text (self->search_entry, "Search your library…");
  g_signal_connect (self->search_entry, "search-changed", G_CALLBACK (on_filters_changed), self);
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->search_entry));

  GtkWidget *rescan_btn = gtk_button_new_from_icon_name ("view-refresh-symbolic");
  gtk_widget_set_tooltip_text (rescan_btn, "Rescan Library");
  g_signal_connect (rescan_btn, "clicked", G_CALLBACK (on_rescan_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), rescan_btn);

  GtkWidget *about_btn = gtk_button_new_from_icon_name ("help-about-symbolic");
  gtk_widget_set_tooltip_text (about_btn, "About");
  g_signal_connect (about_btn, "clicked", G_CALLBACK (on_about_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), about_btn);

  GtkWidget *prefs_btn = gtk_button_new_from_icon_name ("preferences-system-symbolic");
  gtk_widget_set_tooltip_text (prefs_btn, "Preferences");
  g_signal_connect (prefs_btn, "clicked", G_CALLBACK (on_preferences_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), prefs_btn);

  return header;
}

static void
gog_main_window_class_init (GogMainWindowClass *klass)
{
  (void) klass;
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
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (empty_page), "folder-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (empty_page), "No Games Found Here");
  adw_status_page_set_description (ADW_STATUS_PAGE (empty_page),
                                    "By default the current folder is scanned. "
                                    "Choose the folder containing your downloaded GOG games, "
                                    "or set one permanently in Preferences.");
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
  g_signal_connect_swapped (self->detail, GOG_GAME_DETAIL_SIGNAL_CHANGED,
                             G_CALLBACK (on_filters_changed), self);
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
