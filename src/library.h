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

/* Look up a loaded game by product id, or NULL. Transfer none. */
GogGame *gog_library_find_by_id (GogLibrary *self, gint64 id);

G_END_DECLS

#endif
