/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_SHORTCUT_H
#define GOG_SHORTCUT_H

#include <glib.h>
#include "game.h"

G_BEGIN_DECLS

/* An Applications-menu entry for an installed game:
 * ~/.local/share/applications/gog-manager-<id>.desktop, which runs the
 * game's play.sh, so it starts without gog-manager or the library disk.
 * Its icon is copied to ~/.local/share/gog-manager/icons/ for the same
 * reason. */

gchar   *gog_shortcut_path (GogGame *game);
gboolean gog_shortcut_exists (GogGame *game);

/* Writes play.sh (gog_write_play_script) if needed, then the entry. */
gboolean gog_shortcut_create (GogGame *game, GError **error);

/* Removes the entry and its icon; TRUE when there was none. */
gboolean gog_shortcut_remove (GogGame *game, GError **error);

/* The Exec= value that runs path, quoted and escaped as the Desktop Entry
 * spec asks. Exposed for tests. */
gchar *gog_shortcut_exec_value (const char *path);

G_END_DECLS

#endif
