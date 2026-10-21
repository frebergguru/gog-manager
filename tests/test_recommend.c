/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "../src/recommend.h"

#define DAY (24 * 60 * 60)
#define NOW ((gint64) 1792000000) /* 2026-10-14 */

static GStrv
strv (const char *first, ...)
{
  GPtrArray *a = g_ptr_array_new ();
  va_list ap;
  va_start (ap, first);
  for (const char *s = first; s; s = va_arg (ap, const char *))
    g_ptr_array_add (a, g_strdup (s));
  va_end (ap);
  g_ptr_array_add (a, NULL);
  return (GStrv) g_ptr_array_free (a, FALSE);
}

/* A game with these categories, and optionally a series and developer. */
static GogGame *
add_game (GPtrArray *games, gint64 id, const char *title, gboolean installed, GStrv categories,
          const char *series, const char *developer)
{
  GogGame *game = gog_game_new ();
  gog_game_set_id (game, id);
  gog_game_set_title (game, title);
  gog_game_set_supports_os (game, GOG_OS_WINDOWS, TRUE);
  gog_game_set_installed (game, installed);
  gog_game_set_categories (game, categories);
  if (series || developer)
    {
      GogGamesdbInfo *db = g_new0 (GogGamesdbInfo, 1);
      db->series = g_strdup (series);
      db->developers = developer ? strv (developer, NULL) : strv (NULL);
      db->publishers = strv (NULL);
      db->themes = strv (NULL);
      db->game_modes = strv (NULL);
      db->genres = strv (NULL);
      db->screenshots = strv (NULL);
      db->video_ids = strv (NULL);
      db->video_names = strv (NULL);
      gog_game_set_gamesdb (game, db);
    }
  g_ptr_array_add (games, game);
  return game;
}

static GogSuggestion *
find (GPtrArray *suggestions, const char *title)
{
  for (guint i = 0; i < suggestions->len; i++)
    {
      GogSuggestion *s = g_ptr_array_index (suggestions, i);
      if (g_str_equal (gog_game_get_title (s->game), title))
        return s;
    }
  return NULL;
}

static void
test_recommend (void)
{
  GPtrArray *games = g_ptr_array_new_with_free_func (g_object_unref);
  GogGame *saga1 = add_game (games, 1, "Saga One", TRUE, strv ("Role-playing", "Fantasy", NULL),
                             "Saga", "Studio X");
  gog_game_set_play_history (saga1, NOW - DAY, 20 * 3600);
  add_game (games, 2, "Space Thing", TRUE, strv ("Role-playing", "Sci-fi", NULL), NULL, "Studio Y");
  add_game (games, 3, "Saga Two", FALSE, strv ("Role-playing", NULL), "Saga", NULL);
  add_game (games, 4, "Fast Cars", FALSE, strv ("Racing", NULL), NULL, NULL);
  GogGame *mac = add_game (games, 5, "Mac Fantasy", FALSE, strv ("Role-playing", "Fantasy", NULL),
                           NULL, NULL);
  gog_game_set_supports_os (mac, GOG_OS_WINDOWS, FALSE);
  gog_game_set_supports_os (mac, GOG_OS_MAC, TRUE);
  GogGame *dlc = add_game (games, 6, "Saga One DLC", FALSE, strv ("Role-playing", "Fantasy", NULL),
                           "Saga", NULL);
  gog_game_set_base_game (dlc, saga1);
  add_game (games, 7, "Star Quest", FALSE, strv ("Role-playing", "Sci-fi", NULL), NULL, NULL);
  /* Common to both installed games, and to nothing else: shared, but says
   * little. */
  add_game (games, 8, "Plain RPG", FALSE, strv ("Role-playing", NULL), NULL, NULL);

  GPtrArray *mine = gog_recommend_for_you (games, NOW, 10);
  /* Never what is installed, can't be installed, is DLC or shares nothing. */
  g_assert_null (find (mine, "Saga One"));
  g_assert_null (find (mine, "Mac Fantasy"));
  g_assert_null (find (mine, "Saga One DLC"));
  g_assert_null (find (mine, "Fast Cars"));
  g_assert_cmpuint (mine->len, ==, 3);
  GogSuggestion *first = g_ptr_array_index (mine, 0);
  g_assert_cmpstr (gog_game_get_title (first->game), ==, "Saga Two");
  g_assert_true (g_str_has_prefix (first->reason, "Part of the Saga series, like Saga One."));
  GogSuggestion *star = find (mine, "Star Quest");
  g_assert_nonnull (star);
  g_assert_cmpfloat (star->score, >, find (mine, "Plain RPG")->score);
  g_assert_nonnull (strstr (star->reason, "Sci-fi"));
  GPtrArray *one = gog_recommend_for_you (games, NOW, 1);
  g_assert_cmpuint (one->len, ==, 1);
  g_ptr_array_unref (one);
  g_ptr_array_unref (mine);

  /* Played yesterday: not one to play next; never played: is. */
  GPtrArray *next = gog_recommend_play_next (games, NOW, 10);
  g_assert_cmpuint (next->len, ==, 1);
  GogSuggestion *space = g_ptr_array_index (next, 0);
  g_assert_cmpstr (gog_game_get_title (space->game), ==, "Space Thing");
  g_assert_true (g_str_has_prefix (space->reason, "Not played yet. Shares Role-playing with Saga One."));
  g_ptr_array_unref (next);

  GPtrArray *cont = gog_recommend_continue (games, 5);
  g_assert_cmpuint (cont->len, ==, 1);
  g_assert_true (g_ptr_array_index (cont, 0) == saga1);
  g_ptr_array_unref (cont);
  g_ptr_array_unref (games);
}

static void
test_describe_play_history (void)
{
  gchar *text = gog_describe_play_history (0, 0, NOW);
  g_assert_cmpstr (text, ==, "Not played yet");
  g_free (text);
  text = gog_describe_play_history (NOW - 60, 2 * 3600 + 100, NOW);
  g_assert_cmpstr (text, ==, "Last played today · 2 hours played");
  g_free (text);
  text = gog_describe_play_history (NOW - 3 * DAY, 30, NOW);
  g_assert_cmpstr (text, ==, "Last played 3 days ago · less than a minute played");
  g_free (text);
  text = gog_describe_play_history (NOW - 20 * DAY, 60 * 25, NOW);
  g_assert_cmpstr (text, ==, "Last played 2 weeks ago · 25 minutes played");
  g_free (text);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/recommend/suggestions", test_recommend);
  g_test_add_func ("/recommend/describe_play_history", test_describe_play_history);
  return g_test_run ();
}
