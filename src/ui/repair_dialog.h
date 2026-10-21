/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_REPAIR_DIALOG_H
#define GOG_REPAIR_DIALOG_H

#include <adwaita.h>
#include "../game.h"

G_BEGIN_DECLS

/* A window that checks game's downloaded files against GOG's checksums
 * with lgogdownloader --repair (which downloads damaged parts again), shows
 * what it is doing, and says what it found. library_dir is lgogdownloader's
 * download folder; the game's folder in it is named after its gamename. */
void gog_show_repair_dialog (GtkWindow *parent, const char *library_dir, GogGame *game);

G_END_DECLS

#endif
