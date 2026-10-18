/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_LAUNCH_H
#define GOG_LAUNCH_H

#include <glib.h>
#include "game.h"

G_BEGIN_DECLS

/* Launches an installed game. Resolution order:
 *   1. A goggame-*.info manifest in the install directory (GOG's own
 *      format, written by both the Windows and Linux installers) -- the
 *      isPrimary FileTask with category "launcher" (falling back to any
 *      isPrimary FileTask) is what gets run.
 *   2. Linux native fallback: install_dir/start.sh.
 *   3. Windows fallback: the first non-uninstaller *.exe found at the top
 *      of the install directory ("final resort", matching what real GOG
 *      installs look like when no manifest is present).
 * Runs the game via the runner/prefix recorded at install time. Spawns
 * detached (fire-and-forget) and returns immediately; the returned FALSE
 * + error covers "could not even start it", not the game's own exit code. */
gboolean gog_launch_game (GogGame *game, GError **error);

/* A launch whose Wine/Proton prefix has to be (re)built first returns TRUE
 * straight away and finishes on a thread; if it then fails, func gets the
 * message on the main thread. One handler at a time; NULL removes it. */
typedef void (*GogLaunchFailedFunc) (const char *message, gpointer user_data);
void gog_launch_set_failed_func (GogLaunchFailedFunc func, gpointer user_data);

/* Opens winecfg / regedit against a game's existing Wine or Proton prefix.
 * Returns FALSE with error set for natively-installed or not-yet-installed
 * games, or (winecfg only) when the runner is Proton. */
gboolean gog_open_winecfg (GogGame *game, GError **error);
gboolean gog_open_regedit (GogGame *game, GError **error);

G_END_DECLS

#endif
