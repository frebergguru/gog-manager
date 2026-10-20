/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_SYNC_DIALOG_H
#define GOG_SYNC_DIALOG_H

#include <adwaita.h>
#include "../library.h"

G_BEGIN_DECLS

/* Called once the dialog is closed. downloaded is TRUE if anything was
 * downloaded (so the library should be rescanned); the saved check results
 * may have changed either way. */
typedef void (*GogSyncDoneFunc) (gboolean downloaded, gpointer user_data);

/* Presents a dialog that checks GOG (through lgogdownloader) for games not
 * downloaded yet and for new versions of downloaded ones, and downloads the
 * chosen ones into library_dir, deleting the old versions afterwards.
 * library is used for game titles. */
void gog_show_sync_dialog (GtkWindow *parent, const char *library_dir, GogLibrary *library,
                           GogSyncDoneFunc on_done, gpointer user_data);

/* The same, with gamename (a game not downloaded yet) ticked and opened,
 * so its languages and parts can be chosen before Download. */
void gog_show_sync_dialog_for_game (GtkWindow *parent, const char *library_dir,
                                    GogLibrary *library, const char *gamename,
                                    GogSyncDoneFunc on_done, gpointer user_data);

G_END_DECLS

#endif
