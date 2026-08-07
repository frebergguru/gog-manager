/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_GAME_DETAIL_H
#define GOG_GAME_DETAIL_H

#include <adwaita.h>
#include "../game.h"
#include "../library.h"
#include "../config.h"

G_BEGIN_DECLS

#define GOG_TYPE_GAME_DETAIL (gog_game_detail_get_type ())
G_DECLARE_FINAL_TYPE (GogGameDetail, gog_game_detail, GOG, GAME_DETAIL, GtkBox)

/* parent_window is used as the transient parent for the install dialog and
 * file chooser; library lets the DLC tab find/install child products;
 * config/state are shared, mutable app state (config->install_root is
 * read; state is updated and saved on every install/uninstall). */
GogGameDetail *gog_game_detail_new (GtkWindow *parent_window, GogLibrary *library,
                                     GogConfig *config, GogState *state);

void gog_game_detail_set_game (GogGameDetail *self, GogGame *game);

/* Emitted (no args) after an install/uninstall completes, so the owner can
 * re-sort/re-filter the grid (install status is a filter/sort key). */
#define GOG_GAME_DETAIL_SIGNAL_CHANGED "changed"

G_END_DECLS

#endif
