/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_GAMESDB_H
#define GOG_GAMESDB_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* What GOG's games database (gamesdb.gog.com/platforms/gog/
 * external_releases/<id>) says about a game, beyond the product json:
 * artwork, screenshots, videos, a short summary, a rating and more ways to
 * group games. Results, including "not in gamesdb", are cached in the
 * app's config folder in gog-manager-gamesdb.json, keyed by product id,
 * and the cover and background are kept as JPEG files in gamesdb-images/,
 * so all of it works offline. Screenshots join them there when a game page
 * first shows them.
 *
 * Left out: the other stores a game is on (gamesdb links games to stores
 * that don't sell them), languages and systems (the product json has
 * them), DLC (the library links it), and logo and icon (lgogdownloader
 * saves them). */
typedef struct
{
  gchar *summary;        /* plain text; NULL if none */
  gchar *series;         /* e.g. "The Witcher"; NULL if none */
  GStrv developers;      /* never NULL; empty when not known */
  GStrv publishers;
  GStrv themes;          /* e.g. "Open world", "Fantasy" */
  GStrv game_modes;      /* e.g. "Single player", "Co-operative" */
  GStrv genres;          /* e.g. "Role-playing (RPG)" */
  gint64 first_release;  /* the game's first release anywhere (unix time), 0 if not known */
  gdouble rating;        /* critics' average, 0-100; 0 if there is none */
  GStrv screenshots;     /* url_formats of screenshots, then artwork; never NULL */
  GStrv video_ids;       /* YouTube video ids; never NULL */
  GStrv video_names;     /* parallel to video_ids, e.g. "Trailer"; "" when it has none */
  gchar *cover_url;      /* vertical box art (JPEG), NULL if none */
  gchar *background_url; /* wide background (JPEG), NULL if none */
  gchar *cover_path;     /* the downloaded cover; NULL until it is there */
  gchar *background_path;
} GogGamesdbInfo;

void gog_gamesdb_info_free (GogGamesdbInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GogGamesdbInfo, gog_gamesdb_info_free)

/* TRUE when a and b (either may be NULL) would show the same. */
gboolean gog_gamesdb_info_equal (const GogGamesdbInfo *a, const GogGamesdbInfo *b);

/* Called after each step with how much of the work is done: first the
 * details (images FALSE), then the covers and backgrounds (images TRUE),
 * which are on disk for gog_gamesdb_load() as each step ends. */
typedef void (*GogGamesdbProgress) (guint done, guint total, gboolean images, gpointer user_data);

/* Looks up the games not in the cache yet (and those gamesdb didn't know
 * more than a month ago, or that an older version of the app cached with
 * fewer details), then downloads any cover and background that is
 * missing. Blocking (runs curl): call it from a worker thread; progress
 * runs on that thread too. It stops between batches once cancellable is
 * cancelled. A failed lookup is reported in error; what was found is
 * kept. *looked_up (may be NULL) gets how many games were asked for. */
gboolean gog_gamesdb_lookup (const gint64 *ids, guint n_ids, GCancellable *cancellable,
                             GogGamesdbProgress progress, gpointer user_data, guint *looked_up,
                             GError **error);

/* Empties the cache, so the next lookup asks about every game again. The
 * images stay until a lookup finds they are no longer used. Waits for a
 * lookup in progress to save first. */
void gog_gamesdb_forget (void);

/* A screenshot (one of GogGamesdbInfo.screenshots) as a thumbnail for a
 * gallery (510x287) or full size (1600 wide): where it is downloaded from,
 * and where it is kept once it is. NULL for a url_format that isn't
 * GOG's. Transfer full. */
gchar *gog_gamesdb_screenshot_url (const char *url_format, gboolean full);
gchar *gog_gamesdb_screenshot_path (const char *url_format, gboolean full);

/* Downloads the given screenshots that aren't on disk yet, in batches;
 * progress runs after each (images TRUE). Blocking: call it from a worker
 * thread. Stops between batches once cancellable is cancelled. */
gboolean gog_gamesdb_fetch_screenshots (const char *const *url_formats, gboolean full,
                                        GCancellable *cancellable, GogGamesdbProgress progress,
                                        gpointer user_data, GError **error);

/* The cache: product id as a decimal string -> GogGamesdbInfo*, for the
 * games gamesdb knows. Transfer full. */
GHashTable *gog_gamesdb_load (void);

/* One gamesdb answer; NULL if it can't be read. The paths are NULL.
 * Exposed for tests. */
GogGamesdbInfo *gog_gamesdb_parse (const char *json);

G_END_DECLS

#endif
