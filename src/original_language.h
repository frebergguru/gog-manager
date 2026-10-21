/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_ORIGINAL_LANGUAGE_H
#define GOG_ORIGINAL_LANGUAGE_H

#include <glib.h>

G_BEGIN_DECLS

/* Each game's original language, from Wikidata, matched on the game's GOG
 * link ("game/<gamename>", property P2725): its "original language" (P364)
 * if set, otherwise the main language of its country of origin (P495),
 * otherwise of its developer's country (P178 -> P17). So a game made in
 * Norway counts as Norwegian. Results, including "not found", are cached
 * in the app's config folder in gog-manager-original-languages.json. */

/* Looks up the games not in the cache yet, and those not found more than
 * a month ago. Blocking (runs curl): call it from a worker thread. It
 * stops between queries once *cancelled is TRUE (cancelled may be NULL).
 * A failed lookup is reported in error; what was found is kept. */
gboolean gog_original_lookup (const char *const *gamenames, const volatile gboolean *cancelled,
                              GError **error);

/* lgogdownloader language bits of the game's original language(s), 0 if
 * not known. When source is not NULL it gets a short reason ("made in
 * Norway"), or NULL if not known. */
guint gog_original_languages (const char *gamename, gchar **source);

/* The same for every game whose original language is known: gamename ->
 * language bits (GUINT_TO_POINTER). Transfer full. */
GHashTable *gog_original_languages_load (void);

/* For tests: the language bits for an ISO 3166 country code ("NO"), or 0. */
guint gog_original_country_languages (const char *iso_country);

G_END_DECLS

#endif
