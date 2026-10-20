/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_GAMESDB_H
#define GOG_GAMESDB_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* What GOG's games database (gamesdb.gog.com/platforms/gog/
 * external_releases/<id>) says about a game, beyond the product json:
 * artwork, a short summary and more ways to group games. Results,
 * including "not in gamesdb", are cached in the app's config folder in
 * gog-manager-gamesdb.json, keyed by product id, and the cover and
 * background are kept as JPEG files in gamesdb-images/, so all of it works
 * offline. */
typedef struct
{
  gchar *summary;        /* plain text; NULL if none */
  gchar *series;         /* e.g. "The Witcher"; NULL if none */
  GStrv developers;      /* never NULL; empty when not known */
  GStrv publishers;
  GStrv themes;          /* e.g. "Open world", "Fantasy" */
  GStrv game_modes;      /* e.g. "Single player", "Co-operative" */
  gchar *cover_url;      /* vertical box art (JPEG), NULL if none */
  gchar *background_url; /* wide background (JPEG), NULL if none */
  gchar *cover_path;     /* the downloaded cover; NULL until it is there */
  gchar *background_path;
} GogGamesdbInfo;

void gog_gamesdb_info_free (GogGamesdbInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GogGamesdbInfo, gog_gamesdb_info_free)

/* Called after each step with how much of the work is done. */
typedef void (*GogGamesdbProgress) (guint done, guint total, gpointer user_data);

/* Looks up the games not in the cache yet (and those gamesdb didn't know
 * more than a month ago), then downloads any cover and background that is
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

/* The cache: product id as a decimal string -> GogGamesdbInfo*, for the
 * games gamesdb knows. Transfer full. */
GHashTable *gog_gamesdb_load (void);

/* One gamesdb answer; NULL if it can't be read. The paths are NULL.
 * Exposed for tests. */
GogGamesdbInfo *gog_gamesdb_parse (const char *json);

G_END_DECLS

#endif
