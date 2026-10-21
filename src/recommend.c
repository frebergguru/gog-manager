/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "recommend.h"
#include "gog_sync.h"
#include "original_language.h"
#include <math.h>
#include <string.h>

#define DAY (24 * 60 * 60)
/* Installed games not played for this long are offered to play next. */
#define PLAY_NEXT_AFTER (60 * DAY)

/* What two games can have in common, from the strongest sign that they are
 * alike to the weakest. */
typedef enum
{
  KIND_SERIES,
  KIND_DEVELOPER,
  KIND_CATEGORY,
  KIND_THEME,
  KIND_GENRE,
  KIND_TAG,
  KIND_PUBLISHER,
  KIND_LANGUAGE,
  KIND_ERA,
  KIND_MODE,
  N_KINDS,
} Kind;

static const double kind_weight[N_KINDS] = { 3.0, 1.5, 1.0, 0.8, 0.7, 0.6, 0.5, 0.5, 0.4, 0.3 };

/* The kinds told as "Shares A, B and C", together; the others each have a
 * sentence of their own. */
static gboolean
is_shared_kind (Kind kind)
{
  return kind == KIND_CATEGORY || kind == KIND_THEME || kind == KIND_GENRE || kind == KIND_TAG ||
         kind == KIND_MODE;
}

void
gog_suggestion_free (GogSuggestion *suggestion)
{
  if (!suggestion)
    return;
  g_object_unref (suggestion->game);
  g_free (suggestion->reason);
  g_free (suggestion);
}

/* ---- what a game has ---- */

/* "<kind>\x1f<value>" keys */
static gchar *
make_key (Kind kind, const char *value)
{
  return g_strdup_printf ("%d\x1f%s", kind, value);
}

static Kind
key_kind (const char *key)
{
  return (Kind) g_ascii_strtoll (key, NULL, 10);
}

static const char *
key_value (const char *key)
{
  return strchr (key, '\x1f') + 1;
}

static void
add_values (GHashTable *set, Kind kind, const char *const *values)
{
  for (const char *const *v = values; v && *v; v++)
    if (**v)
      g_hash_table_add (set, make_key (kind, *v));
}

static gint64
release_of (GogGame *game)
{
  const GogGamesdbInfo *db = gog_game_get_gamesdb (game);
  if (db && db->first_release > 0)
    return db->first_release;
  return gog_game_get_release_date (game);
}

/* Every value the game has, as keys. languages: gog_original_languages_load().
 * Transfer full. */
static GHashTable *
game_signals (GogGame *game, GHashTable *languages)
{
  GHashTable *set = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  const GogGamesdbInfo *db = gog_game_get_gamesdb (game);
  const GogStoreInfo *store = gog_game_get_store_info (game);
  add_values (set, KIND_CATEGORY, gog_game_get_categories (game));
  if (db)
    {
      if (db->series && *db->series)
        g_hash_table_add (set, make_key (KIND_SERIES, db->series));
      add_values (set, KIND_DEVELOPER, (const char *const *) db->developers);
      add_values (set, KIND_PUBLISHER, (const char *const *) db->publishers);
      add_values (set, KIND_THEME, (const char *const *) db->themes);
      add_values (set, KIND_GENRE, (const char *const *) db->genres);
      add_values (set, KIND_MODE, (const char *const *) db->game_modes);
    }
  if (store)
    {
      /* Not the features: achievements, cloud saves and the like say
       * nothing about the game itself. */
      add_values (set, KIND_TAG, (const char *const *) store->tags);
    }
  guint original = gog_game_get_gamename (game)
                       ? GPOINTER_TO_UINT (g_hash_table_lookup (languages, gog_game_get_gamename (game)))
                       : 0;
  if (original)
    {
      gchar *names = gog_sync_describe_languages (original);
      g_hash_table_add (set, make_key (KIND_LANGUAGE, names));
      g_free (names);
    }
  gint64 released = release_of (game);
  if (released > 0)
    {
      GDateTime *date = g_date_time_new_from_unix_utc (released);
      if (date)
        {
          gchar *decade = g_strdup_printf ("%ds", g_date_time_get_year (date) / 10 * 10);
          g_hash_table_add (set, make_key (KIND_ERA, decade));
          g_free (decade);
          g_date_time_unref (date);
        }
    }
  return set;
}

/* ---- the library ---- */

typedef struct
{
  GogGame *game;
  GHashTable *signals;
  double weight; /* in the profile: more for games played more, and lately */
} Entry;

static void
entry_free (Entry *entry)
{
  g_hash_table_unref (entry->signals);
  g_free (entry);
}

static gboolean
installable (GogGame *game)
{
  return gog_game_get_supports_os (game, GOG_OS_WINDOWS) ||
         gog_game_get_supports_os (game, GOG_OS_LINUX);
}

static double
play_weight (GogGame *game, gint64 now)
{
  double hours = gog_game_get_play_seconds (game) / 3600.0;
  double weight = 1.0 + MIN (hours, 40.0) / 10.0;
  gint64 last = gog_game_get_last_played (game);
  if (last > 0)
    weight += now - last < 30 * DAY ? 1.5 : now - last < 180 * DAY ? 0.5 : 0.0;
  return weight;
}

typedef struct
{
  GPtrArray *entries;   /* Entry*, the games (not DLC) */
  GHashTable *idf;      /* key -> double*: rarer values count more */
} Library;

static Library *
library_new (GPtrArray *games, gint64 now)
{
  Library *lib = g_new0 (Library, 1);
  lib->entries = g_ptr_array_new_with_free_func ((GDestroyNotify) entry_free);
  GHashTable *df = g_hash_table_new (g_str_hash, g_str_equal);
  GHashTable *languages = gog_original_languages_load ();
  for (guint i = 0; i < games->len; i++)
    {
      GogGame *game = g_ptr_array_index (games, i);
      if (gog_game_get_base_game (game) || gog_game_get_product_type (game) == GOG_PRODUCT_DLC)
        continue;
      Entry *entry = g_new0 (Entry, 1);
      entry->game = game;
      entry->signals = game_signals (game, languages);
      entry->weight = play_weight (game, now);
      g_ptr_array_add (lib->entries, entry);
      GHashTableIter it;
      gpointer key;
      g_hash_table_iter_init (&it, entry->signals);
      while (g_hash_table_iter_next (&it, &key, NULL))
        g_hash_table_insert (df, key, GUINT_TO_POINTER (GPOINTER_TO_UINT (g_hash_table_lookup (df, key)) + 1));
    }
  lib->idf = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  double n = lib->entries->len;
  GHashTableIter it;
  gpointer key, count;
  g_hash_table_iter_init (&it, df);
  while (g_hash_table_iter_next (&it, &key, &count))
    {
      double *idf = g_new (double, 1);
      *idf = log (1.0 + n / GPOINTER_TO_UINT (count));
      g_hash_table_insert (lib->idf, g_strdup (key), idf);
    }
  g_hash_table_unref (df); /* its keys are the entries' */
  g_hash_table_unref (languages);
  return lib;
}

static void
library_free (Library *lib)
{
  g_ptr_array_unref (lib->entries);
  g_hash_table_unref (lib->idf);
  g_free (lib);
}

/* ---- comparing a game with the installed ones ---- */

typedef struct
{
  const char *key;
  double value;
} Contribution;

static gint
compare_contributions (gconstpointer a, gconstpointer b)
{
  const Contribution *ca = a, *cb = b;
  if (ca->value != cb->value)
    return ca->value > cb->value ? -1 : 1;
  return g_strcmp0 (ca->key, cb->key);
}

/* The installed games other than candidate. */
static gboolean
in_profile (Entry *entry, Entry *candidate)
{
  return entry != candidate && gog_game_get_installed (entry->game);
}

/* How much candidate shares with the installed games, and how much each of
 * its values counted (Contribution, sorted, largest first). */
static double
score (Library *lib, Entry *candidate, GArray *contributions)
{
  double total_weight = 0;
  for (guint i = 0; i < lib->entries->len; i++)
    {
      Entry *e = g_ptr_array_index (lib->entries, i);
      if (in_profile (e, candidate))
        total_weight += e->weight;
    }
  if (total_weight <= 0)
    return 0;

  double sum = 0;
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init (&it, candidate->signals);
  while (g_hash_table_iter_next (&it, &key, NULL))
    {
      double shared = 0;
      for (guint i = 0; i < lib->entries->len; i++)
        {
          Entry *e = g_ptr_array_index (lib->entries, i);
          if (in_profile (e, candidate) && g_hash_table_contains (e->signals, key))
            shared += e->weight;
        }
      if (shared <= 0)
        continue;
      double *idf = g_hash_table_lookup (lib->idf, key);
      Contribution c = { key, kind_weight[key_kind (key)] * (idf ? *idf : 1.0) * shared / total_weight };
      g_array_append_val (contributions, c);
      sum += c.value;
    }
  g_array_sort (contributions, compare_contributions);
  /* So that games with many tags don't win on numbers alone. */
  return sum / sqrt (MAX (g_hash_table_size (candidate->signals), 4));
}

/* The installed game, other than candidate, with value that was played
 * most; NULL if none. */
static GogGame *
best_with (Library *lib, Entry *candidate, const char *key)
{
  Entry *best = NULL;
  for (guint i = 0; i < lib->entries->len; i++)
    {
      Entry *e = g_ptr_array_index (lib->entries, i);
      if (in_profile (e, candidate) && g_hash_table_contains (e->signals, key) &&
          (!best || e->weight > best->weight))
        best = e;
    }
  return best ? best->game : NULL;
}

/* How many installed games, other than candidate, have any of keys. */
static guint
count_with (Library *lib, Entry *candidate, GPtrArray *keys)
{
  guint n = 0;
  for (guint i = 0; i < lib->entries->len; i++)
    {
      Entry *e = g_ptr_array_index (lib->entries, i);
      if (!in_profile (e, candidate))
        continue;
      for (guint k = 0; k < keys->len; k++)
        if (g_hash_table_contains (e->signals, g_ptr_array_index (keys, k)))
          {
            n++;
            break;
          }
    }
  return n;
}

/* "A, B and C". Transfer full. */
static gchar *
join_values (GPtrArray *keys)
{
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < keys->len; i++)
    {
      if (i > 0)
        g_string_append (out, i + 1 == keys->len ? " and " : ", ");
      g_string_append (out, key_value (g_ptr_array_index (keys, i)));
    }
  return g_string_free (out, FALSE);
}

/* The length of value without a " (RPG)" at its end. */
static gsize
bare_length (const char *value)
{
  const char *paren = strstr (value, " (");
  return paren ? (gsize) (paren - value) : strlen (value);
}

/* TRUE if keys has one with the same value as key, ignoring case and a
 * part in brackets: the games database's "Role-playing (RPG)" is GOG's
 * "Role-playing". */
static gboolean
has_value (GPtrArray *keys, const char *key)
{
  const char *value = key_value (key);
  for (guint i = 0; i < keys->len; i++)
    {
      const char *other = key_value (g_ptr_array_index (keys, i));
      gsize n = bare_length (value);
      if (n == bare_length (other) && g_ascii_strncasecmp (value, other, n) == 0)
        return TRUE;
    }
  return FALSE;
}

/* One sentence on what candidate shares through kind (any shared kind for
 * those). Transfer full. */
static gchar *
kind_sentence (Library *lib, Entry *candidate, GArray *contributions, Kind kind)
{
  /* Up to three values for the kinds a game has several of. */
  gboolean single = kind == KIND_SERIES || kind == KIND_LANGUAGE || kind == KIND_ERA;
  GPtrArray *keys = g_ptr_array_new ();
  for (guint i = 0; i < contributions->len && keys->len < (single ? 1u : 3u); i++)
    {
      Contribution *c = &g_array_index (contributions, Contribution, i);
      Kind k = key_kind (c->key);
      if ((k == kind || (is_shared_kind (k) && is_shared_kind (kind))) && !has_value (keys, c->key))
        g_ptr_array_add (keys, (gpointer) c->key);
    }
  const char *first = key_value (g_ptr_array_index (keys, 0));
  GogGame *like = best_with (lib, candidate, g_ptr_array_index (keys, 0));
  const char *like_title = like ? gog_game_get_title (like) : "";
  guint n = count_with (lib, candidate, keys);
  gchar *values = join_values (keys);
  gchar *text;
  switch (kind)
    {
    case KIND_SERIES:
      text = g_strdup_printf ("Part of the %s series, like %s.", first, like_title);
      break;
    case KIND_DEVELOPER:
      text = g_strdup_printf ("Made by %s, like %s.", values, like_title);
      break;
    case KIND_PUBLISHER:
      text = g_strdup_printf ("Published by %s, like %s.", values, like_title);
      break;
    case KIND_LANGUAGE:
      text = n == 1 ? g_strdup_printf ("Originally in %s, like %s.", first, like_title)
                    : g_strdup_printf ("Originally in %s, like %u of your installed games.", first, n);
      break;
    case KIND_ERA:
      text = n == 1 ? g_strdup_printf ("From the %s, like %s.", first, like_title)
                    : g_strdup_printf ("From the %s, like %u of your installed games.", first, n);
      break;
    default:
      text = n == 1 ? g_strdup_printf ("Shares %s with %s.", values, like_title)
                    : g_strdup_printf ("Shares %s with %u of your installed games.", values, n);
      break;
    }
  g_free (values);
  g_ptr_array_unref (keys);
  return text;
}

/* The one or two kinds that counted most, as sentences; NULL when nothing
 * is shared. Transfer full. */
static gchar *
overlap_reason (Library *lib, Entry *candidate, GArray *contributions)
{
  /* The shared kinds count as one, under KIND_CATEGORY. */
  double by_kind[N_KINDS] = { 0 };
  for (guint i = 0; i < contributions->len; i++)
    {
      Contribution *c = &g_array_index (contributions, Contribution, i);
      Kind k = key_kind (c->key);
      by_kind[is_shared_kind (k) ? KIND_CATEGORY : k] += c->value;
    }
  Kind top = N_KINDS, second = N_KINDS;
  for (Kind k = 0; k < N_KINDS; k++)
    {
      if (by_kind[k] <= 0)
        continue;
      if (top == N_KINDS || by_kind[k] > by_kind[top])
        {
          second = top;
          top = k;
        }
      else if (second == N_KINDS || by_kind[k] > by_kind[second])
        second = k;
    }
  if (top == N_KINDS)
    return NULL;
  gchar *text = kind_sentence (lib, candidate, contributions, top);
  if (second != N_KINDS && by_kind[second] >= by_kind[top] / 2)
    {
      gchar *more = kind_sentence (lib, candidate, contributions, second);
      gchar *both = g_strdup_printf ("%s %s", text, more);
      g_free (more);
      g_free (text);
      text = both;
    }
  return text;
}

static gint
compare_suggestions (gconstpointer a, gconstpointer b)
{
  const GogSuggestion *sa = *(GogSuggestion *const *) a;
  const GogSuggestion *sb = *(GogSuggestion *const *) b;
  if (sa->score != sb->score)
    return sa->score > sb->score ? -1 : 1;
  return g_utf8_collate (gog_game_get_title (sa->game), gog_game_get_title (sb->game));
}

static GPtrArray *
recommend (GPtrArray *games, gint64 now, guint max, gboolean play_next)
{
  Library *lib = library_new (games, now);
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_suggestion_free);
  for (guint i = 0; i < lib->entries->len; i++)
    {
      Entry *e = g_ptr_array_index (lib->entries, i);
      gint64 last = gog_game_get_last_played (e->game);
      gboolean wanted = play_next ? gog_game_get_installed (e->game) &&
                                        (last == 0 || now - last >= PLAY_NEXT_AFTER)
                                  : !gog_game_get_installed (e->game) && installable (e->game) &&
                                        gog_game_get_id (e->game) != 0;
      if (!wanted)
        continue;
      GArray *contributions = g_array_new (FALSE, FALSE, sizeof (Contribution));
      double s = score (lib, e, contributions);
      gchar *overlap = s > 0 ? overlap_reason (lib, e, contributions) : NULL;
      g_array_unref (contributions);
      /* A game to install has to share something to be suggested; one
       * already installed is worth a reminder either way. */
      if (!play_next && !overlap)
        continue;
      GogSuggestion *sug = g_new0 (GogSuggestion, 1);
      sug->game = g_object_ref (e->game);
      sug->score = s;
      if (play_next)
        {
          gchar *when = gog_describe_play_history (last, gog_game_get_play_seconds (e->game), now);
          sug->reason = overlap ? g_strdup_printf ("%s. %s", when, overlap) : g_strdup_printf ("%s.", when);
          g_free (when);
          g_free (overlap);
        }
      else
        sug->reason = overlap;
      g_ptr_array_add (out, sug);
    }
  library_free (lib);
  g_ptr_array_sort (out, compare_suggestions);
  if (out->len > max)
    g_ptr_array_set_size (out, max);
  return out;
}

GPtrArray *
gog_recommend_for_you (GPtrArray *games, gint64 now, guint max)
{
  return recommend (games, now, max, FALSE);
}

GPtrArray *
gog_recommend_play_next (GPtrArray *games, gint64 now, guint max)
{
  return recommend (games, now, max, TRUE);
}

static gint
compare_last_played (gconstpointer a, gconstpointer b)
{
  gint64 la = gog_game_get_last_played (*(GogGame *const *) a);
  gint64 lb = gog_game_get_last_played (*(GogGame *const *) b);
  return la == lb ? 0 : la > lb ? -1 : 1;
}

GPtrArray *
gog_recommend_continue (GPtrArray *games, guint max)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < games->len; i++)
    {
      GogGame *game = g_ptr_array_index (games, i);
      if (gog_game_get_installed (game) && !gog_game_get_base_game (game) &&
          gog_game_get_last_played (game) > 0)
        g_ptr_array_add (out, g_object_ref (game));
    }
  g_ptr_array_sort (out, compare_last_played);
  if (out->len > max)
    g_ptr_array_set_size (out, max);
  return out;
}

/* "12 hours played"; NULL for none. Transfer full. */
static gchar *
describe_play_time (gint64 seconds)
{
  if (seconds <= 0)
    return NULL;
  if (seconds < 60)
    return g_strdup ("less than a minute played");
  if (seconds < 3600)
    {
      gint64 minutes = seconds / 60;
      return g_strdup_printf ("%" G_GINT64_FORMAT " %s played", minutes,
                              minutes == 1 ? "minute" : "minutes");
    }
  gint64 hours = (seconds + 1800) / 3600;
  return g_strdup_printf ("%" G_GINT64_FORMAT " %s played", hours, hours == 1 ? "hour" : "hours");
}

gchar *
gog_describe_play_history (gint64 last_played, gint64 seconds, gint64 now)
{
  if (last_played <= 0)
    return g_strdup ("Not played yet");
  gchar *when;
  GDateTime *then = g_date_time_new_from_unix_local (last_played);
  GDateTime *today = g_date_time_new_from_unix_local (now);
  /* Calendar days, not 24-hour periods. */
  GDate d_then, d_today;
  g_date_set_dmy (&d_then, g_date_time_get_day_of_month (then), g_date_time_get_month (then),
                  g_date_time_get_year (then));
  g_date_set_dmy (&d_today, g_date_time_get_day_of_month (today), g_date_time_get_month (today),
                  g_date_time_get_year (today));
  gint days = g_date_days_between (&d_then, &d_today);
  if (days <= 0)
    when = g_strdup ("Last played today");
  else if (days == 1)
    when = g_strdup ("Last played yesterday");
  else if (days < 14)
    when = g_strdup_printf ("Last played %d days ago", days);
  else if (days < 60)
    when = g_strdup_printf ("Last played %d weeks ago", days / 7);
  else
    {
      gchar *month = g_date_time_format (then, "%B %Y");
      when = g_strdup_printf ("Last played in %s", month);
      g_free (month);
    }
  g_date_time_unref (then);
  g_date_time_unref (today);
  gchar *time = describe_play_time (seconds);
  if (!time)
    return when;
  gchar *text = g_strdup_printf ("%s · %s", when, time);
  g_free (time);
  g_free (when);
  return text;
}
