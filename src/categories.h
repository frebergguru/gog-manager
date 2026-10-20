/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_CATEGORIES_H
#define GOG_CATEGORIES_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Each game's categories: the genres GOG lists for it, main one first
 * (api.gog.com/v2/games/<id>, _embedded.tags). Results, including "not on
 * GOG", are cached next to the app in gog-manager-categories.json, keyed
 * by product id, so sorting and filtering work offline. */

/* Called after each batch with how many of the games to look up are done. */
typedef void (*GogCategoriesProgress) (guint done, guint total, gpointer user_data);

/* Looks up the games not in the cache yet, and those GOG didn't know more
 * than a month ago. Blocking (runs curl): call it from a worker thread;
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

/* The categories in one api.gog.com/v2/games answer, in GOG's order; NULL
 * if it can't be read. Exposed for tests. */
GStrv gog_categories_parse (const char *json);

G_END_DECLS

#endif
