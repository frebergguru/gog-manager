/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_RECOMMEND_H
#define GOG_RECOMMEND_H

#include <glib.h>
#include "game.h"

G_BEGIN_DECLS

/* Suggestions from the library alone: games the user owns, never the
 * store's. A game is compared with the installed ones by what is known
 * about it offline (GOG's categories, tags and features, the games
 * database's genres, themes, game modes, series, developers and
 * publishers, its original language and its decade), each shared value
 * counting more the rarer it is in the library and the more, and the more
 * lately, the games that have it were played. */

typedef struct
{
  GogGame *game;   /* reffed */
  gchar *reason;   /* why, as a sentence or two: "Shares Role-playing, Fantasy
                    * with 3 of your installed games." */
  gdouble score;
} GogSuggestion;

void gog_suggestion_free (GogSuggestion *suggestion);

/* Owned games that aren't installed (and can be, on Windows or Linux),
 * most like the installed ones first; only those that share something with
 * them. games: GogGame*, DLC (with a base game) left out. At most max.
 * Element type: GogSuggestion*, transfer full. */
GPtrArray *gog_recommend_for_you (GPtrArray *games, gint64 now, guint max);

/* Installed games not played in the last two months, or never, most like
 * the other installed games (and those played most) first. The reason
 * starts with when it was last played. Transfer full. */
GPtrArray *gog_recommend_play_next (GPtrArray *games, gint64 now, guint max);

/* Installed games that have been played, last played first. At most max.
 * Element type: GogGame*, transfer full (reffed). */
GPtrArray *gog_recommend_continue (GPtrArray *games, guint max);

/* "Last played yesterday · 12 hours played"; "Not played yet" when
 * last_played is 0. Transfer full. */
gchar *gog_describe_play_history (gint64 last_played, gint64 seconds, gint64 now);

G_END_DECLS

#endif
