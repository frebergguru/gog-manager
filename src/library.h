/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_LIBRARY_H
#define GOG_LIBRARY_H

#include <gio/gio.h>
#include "game.h"

G_BEGIN_DECLS

#define GOG_TYPE_LIBRARY (gog_library_get_type ())
G_DECLARE_FINAL_TYPE (GogLibrary, gog_library, GOG, LIBRARY, GObject)

GogLibrary *gog_library_new (void);

/* GListModel<GogGame>, transfer none. Stable for the lifetime of self;
 * scanning clears and repopulates it in place. */
GListModel *gog_library_get_model (GogLibrary *self);

/* Scans root_path (one subdirectory per product) off the main thread.
 * Emits "scan-progress" (double fraction 0..1, string message) on the
 * main thread as it goes. Resolves DLC->base-game links after the scan
 * completes. Any per-game parse failure is logged and skipped, not fatal. */
void     gog_library_scan_async  (GogLibrary *self, const char *root_path,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data);
gboolean gog_library_scan_finish (GogLibrary *self, GAsyncResult *result, GError **error);

/* Sorted, de-duplicated union of every language string seen across the
 * currently loaded games. Transfer full. */
GStrv gog_library_get_all_languages (GogLibrary *self);

/* Reads the language tag GOG puts in extra-language installer names, e.g.
 * "(german)" or "(french_64bit)". Returns FALSE for untagged (English)
 * names. The strings are GOG's language code and native name, static. */
gboolean gog_library_language_from_installer_name (const char *filename, const char **out_code,
                                                   const char **out_name);

/* lgogdownloader's name for a game ("beneath_a_steel_sky") from the name
 * of one of its GOG installers ("setup_beneath_a_steel_sky_1.0_(20270).exe",
 * "beneath_a_steel_sky_en_gog_2_20150.sh"...), or NULL when the file isn't
 * named like a GOG installer. Transfer full. */
gchar *gog_library_gamename_from_installer (const char *filename);

/* Whether game's folder is in library_dir but not named after its
 * gamename, where lgogdownloader looks for it: a download would then make
 * a second copy, and a file check would find nothing. */
gboolean gog_library_folder_misnamed (GogGame *game, const char *library_dir);

/* Renames a misnamed game folder to its gamename, and updates the paths of
 * the loaded games in it (the game, its DLC, installers and extras). Fails
 * when a folder of that name is already there. */
gboolean gog_library_rename_folder (GogLibrary *self, GogGame *game, GError **error);

/* The downloaded, loaded game (not DLC) with this gamename, or NULL.
 * Transfer none. */
GogGame *gog_library_find_by_gamename (GogLibrary *self, const char *gamename);

/* Look up a loaded game by product id, or NULL. Transfer none. */
GogGame *gog_library_find_by_id (GogLibrary *self, gint64 id);

/* Adds a not-downloaded game (gog_game_get_downloaded() FALSE) for each
 * GOG_SYNC_NEW_GAME item of the last check (GogSyncItem*) that the library
 * has no folder or product of, and takes away the ones added before that
 * are no longer in items. */
void gog_library_set_not_downloaded (GogLibrary *self, GPtrArray *items);

G_END_DECLS

#endif
