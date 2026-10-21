/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "for_you.h"
#include "../recommend.h"

/* Rows in each group. */
#define N_CONTINUE 6
#define N_RECOMMENDED 12
#define N_PLAY_NEXT 8

struct _GogForYou
{
  GtkBox parent_instance;

  AdwPreferencesPage *page;
  AdwPreferencesGroup *groups[3];
  GPtrArray *rows[3]; /* the rows each group has now */
};

enum
{
  GROUP_CONTINUE,
  GROUP_RECOMMENDED,
  GROUP_PLAY_NEXT,
};

static guint open_game_signal;

G_DEFINE_FINAL_TYPE (GogForYou, gog_for_you, GTK_TYPE_BOX)

static void
gog_for_you_finalize (GObject *object)
{
  GogForYou *self = GOG_FOR_YOU (object);
  for (guint i = 0; i < G_N_ELEMENTS (self->rows); i++)
    g_ptr_array_unref (self->rows[i]);
  G_OBJECT_CLASS (gog_for_you_parent_class)->finalize (object);
}

static void
gog_for_you_class_init (GogForYouClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = gog_for_you_finalize;
  open_game_signal = g_signal_new (GOG_FOR_YOU_SIGNAL_OPEN_GAME, G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
                                   GOG_TYPE_GAME);
}

static void
gog_for_you_init (GogForYou *self)
{
  for (guint i = 0; i < G_N_ELEMENTS (self->rows); i++)
    self->rows[i] = g_ptr_array_new ();
}

static void
on_row_activated (AdwActionRow *row, gpointer user_data)
{
  GogGame *game = g_object_get_data (G_OBJECT (row), "game");
  if (game)
    g_signal_emit (user_data, open_game_signal, 0, game);
}

static void
clear_group (GogForYou *self, guint group)
{
  for (guint i = 0; i < self->rows[group]->len; i++)
    adw_preferences_group_remove (self->groups[group], g_ptr_array_index (self->rows[group], i));
  g_ptr_array_set_size (self->rows[group], 0);
}

static void
add_row (GogForYou *self, guint group, GogGame *game, const char *subtitle)
{
  GtkWidget *row = adw_action_row_new ();
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), gog_game_get_title (game));
  adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_action_row_set_subtitle_lines (ADW_ACTION_ROW (row), 0);
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
  gtk_widget_set_tooltip_text (row, "Open the game's page");

  /* The box art, small; the square icon when there is none. Decoration
   * only: the title says which game it is. */
  const GogGamesdbInfo *db = gog_game_get_gamesdb (game);
  const char *cover = db && db->cover_path ? db->cover_path : gog_game_get_icon_path (game);
  GtkWidget *picture = gtk_picture_new_for_filename (cover);
  gtk_picture_set_content_fit (GTK_PICTURE (picture),
                               db && db->cover_path ? GTK_CONTENT_FIT_COVER : GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (picture, 40, 56);
  gtk_widget_set_valign (picture, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (picture, "gog-card-cover");
  gtk_accessible_update_property (GTK_ACCESSIBLE (picture), GTK_ACCESSIBLE_PROPERTY_LABEL, "", -1);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), picture);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), gtk_image_new_from_icon_name ("go-next-symbolic"));

  g_object_set_data_full (G_OBJECT (row), "game", g_object_ref (game), g_object_unref);
  g_signal_connect (row, "activated", G_CALLBACK (on_row_activated), self);
  adw_preferences_group_add (self->groups[group], row);
  g_ptr_array_add (self->rows[group], row);
}

static void
set_description (GogForYou *self, guint group, gboolean empty, const char *usual, const char *none)
{
  adw_preferences_group_set_description (self->groups[group], empty ? none : usual);
}

void
gog_for_you_update (GogForYou *self, GPtrArray *games)
{
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  clear_group (self, GROUP_CONTINUE);
  GPtrArray *recent = gog_recommend_continue (games, N_CONTINUE);
  for (guint i = 0; i < recent->len; i++)
    {
      GogGame *game = g_ptr_array_index (recent, i);
      gchar *text = gog_describe_play_history (gog_game_get_last_played (game),
                                               gog_game_get_play_seconds (game), now);
      add_row (self, GROUP_CONTINUE, game, text);
      g_free (text);
    }
  set_description (self, GROUP_CONTINUE, recent->len == 0, "The installed games played last.",
                   "Nothing yet. The games you start with Play in GOG Manager show up here, with "
                   "how long you have played them.");
  g_ptr_array_unref (recent);

  struct
  {
    guint group;
    GPtrArray *(*func) (GPtrArray *, gint64, guint);
    guint max;
    const char *usual;
    const char *none;
  } lists[] = {
    { GROUP_RECOMMENDED, gog_recommend_for_you, N_RECOMMENDED,
      "Games you own but haven't installed, most like the installed ones you play first.",
      "Nothing to suggest yet. Suggestions compare the games you own with the installed ones, "
      "using GOG's details about them, which are fetched after a scan." },
    { GROUP_PLAY_NEXT, gog_recommend_play_next, N_PLAY_NEXT,
      "Installed games you haven't played, or not for two months.",
      "Every installed game has been played in the last two months." },
  };
  for (guint l = 0; l < G_N_ELEMENTS (lists); l++)
    {
      clear_group (self, lists[l].group);
      GPtrArray *found = lists[l].func (games, now, lists[l].max);
      for (guint i = 0; i < found->len; i++)
        {
          GogSuggestion *s = g_ptr_array_index (found, i);
          add_row (self, lists[l].group, s->game, s->reason);
        }
      set_description (self, lists[l].group, found->len == 0, lists[l].usual, lists[l].none);
      g_ptr_array_unref (found);
    }
}

GtkWidget *
gog_for_you_new (void)
{
  GogForYou *self = g_object_new (GOG_TYPE_FOR_YOU, "orientation", GTK_ORIENTATION_VERTICAL, NULL);
  self->page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  adw_preferences_page_set_description (self->page,
                                        "From the games you own only, and how you play them: "
                                        "nothing from GOG's store, and nothing leaves this "
                                        "computer.");
  gtk_widget_set_vexpand (GTK_WIDGET (self->page), TRUE);
  const char *titles[] = { "Continue Playing", "Recommended for You", "What to Play Next" };
  for (guint i = 0; i < G_N_ELEMENTS (titles); i++)
    {
      self->groups[i] = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
      adw_preferences_group_set_title (self->groups[i], titles[i]);
      adw_preferences_page_add (self->page, self->groups[i]);
    }
  gtk_box_append (GTK_BOX (self), GTK_WIDGET (self->page));
  return GTK_WIDGET (self);
}
