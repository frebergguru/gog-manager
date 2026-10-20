/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_CATEGORIES_H
#define GOG_CATEGORIES_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* What GOG's store API (api.gog.com/v2/games/<id>) says about each game:
 * its categories (the genres GOG lists for it, main one first,
 * _embedded.tags) and the rest of its store page in GogStoreInfo. Results,
 * including "not on GOG", are cached in the app's config folder in
 * gog-manager-categories.json, keyed by product id, so sorting and
 * filtering work offline.
 *
 * Left out, as the product json or GOG's games database already give them:
 * description, screenshots, videos, developers, publishers, series,
 * release dates, bonuses, images, and the prices (the games are owned). */

/* One set of system requirements, e.g. Windows, minimum. */
typedef struct
{
  gchar *os;    /* "Windows", "macOS", "Linux" */
  gchar *level; /* "Minimum", "Recommended" */
  GStrv names;  /* e.g. "Processor", parallel to values */
  GStrv values; /* e.g. "Intel Core i5-2400" */
} GogRequirements;

typedef struct
{
  GStrv features;        /* e.g. "Cloud saves", "Controller support" */
  GStrv tags;            /* GOG's store tags, e.g. "Atmospheric", "Sci-fi" */
  GStrv audio_languages; /* e.g. "English", "German" */
  GStrv text_languages;
  GStrv age_ratings;     /* e.g. "PEGI 18 (Bad Language, Violence)", "ESRB Mature 17+" */
  GStrv editions;        /* only when GOG sells more than one */
  GStrv technologies;    /* e.g. "DOSBox", "ScummVM" */
  GPtrArray *requirements; /* GogRequirements*, in GOG's order */
  gchar *copyright;      /* plain text; NULL if none */
  gboolean in_development;
} GogStoreInfo;

void gog_store_info_free (GogStoreInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (GogStoreInfo, gog_store_info_free)

/* TRUE when a and b (either may be NULL) would show the same. */
gboolean gog_store_info_equal (const GogStoreInfo *a, const GogStoreInfo *b);

/* Called after each batch with how many of the games to look up are done. */
typedef void (*GogCategoriesProgress) (guint done, guint total, gpointer user_data);

/* Looks up the games not in the cache yet, those GOG didn't know more
 * than a month ago, and those an older version of the app cached with
 * fewer details. Blocking (runs curl): call it from a worker thread;
 * progress runs on that thread too. It stops between batches once
 * cancellable is cancelled. A failed lookup is reported in error; what was
 * found is kept. *looked_up (may be NULL) gets how many were asked for. */
gboolean gog_categories_lookup (const gint64 *ids, guint n_ids, GCancellable *cancellable,
                                GogCategoriesProgress progress, gpointer user_data,
                                guint *looked_up, GError **error);

/* Empties the cache, so the next lookup asks GOG about every game again.
 * Waits for a lookup in progress to finish first. */
void gog_categories_forget (void);

/* The cache: product id as a decimal string -> GStrv (empty when GOG
 * listed none). Transfer full. */
GHashTable *gog_categories_load (void);

/* The rest: product id as a decimal string -> GogStoreInfo*, for the games
 * GOG's store knows. Transfer full. */
GHashTable *gog_store_info_load (void);

/* The categories in one api.gog.com/v2/games answer, in GOG's order; NULL
 * if it can't be read. Exposed for tests. */
GStrv gog_categories_parse (const char *json);

/* The rest of one answer; NULL if it can't be read. Exposed for tests. */
GogStoreInfo *gog_store_info_parse (const char *json);

G_END_DECLS

#endif
