/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_FOR_YOU_H
#define GOG_FOR_YOU_H

#include <adwaita.h>
#include "../game.h"

G_BEGIN_DECLS

#define GOG_TYPE_FOR_YOU (gog_for_you_get_type ())
G_DECLARE_FINAL_TYPE (GogForYou, gog_for_you, GOG, FOR_YOU, GtkBox)

/* The For You page: Continue Playing, Recommended for You and What to Play
 * Next (see recommend.h), each game a row with the reason it is there. */
GtkWidget *gog_for_you_new (void);

/* Fills the page from games (GogGame*, the whole library). */
void gog_for_you_update (GogForYou *self, GPtrArray *games);

/* "open-game" (GogGame *game): a row was activated. */
#define GOG_FOR_YOU_SIGNAL_OPEN_GAME "open-game"

G_END_DECLS

#endif
