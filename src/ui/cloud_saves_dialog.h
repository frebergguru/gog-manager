/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_CLOUD_SAVES_DIALOG_H
#define GOG_CLOUD_SAVES_DIALOG_H

#include <adwaita.h>
#include "../game.h"

G_BEGIN_DECLS

/* TRUE when game's saves can go to and from GOG Galaxy's cloud: a game
 * (not DLC) known to GOG, installed from its Windows installer in a Wine or
 * Proton prefix. */
gboolean gog_cloud_saves_available (GogGame *game);

/* A window that lists game's saves on GOG (lgogdownloader
 * --galaxy-show-cloud-saves) and downloads or uploads them. */
void gog_show_cloud_saves_dialog (GtkWindow *parent, GogGame *game);

G_END_DECLS

#endif
