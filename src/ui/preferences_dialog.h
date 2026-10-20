/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_PREFERENCES_DIALOG_H
#define GOG_PREFERENCES_DIALOG_H

#include <adwaita.h>
#include "../config.h"
#include "../library.h"

G_BEGIN_DECLS

/* Called after Preferences is closed if the library path changed, so the
 * caller can trigger a rescan. */
typedef void (*GogLibraryPathChangedFunc) (const char *new_path, gpointer user_data);

/* Called when "Fetch Categories Again" is pressed. */
typedef void (*GogRefetchCategoriesFunc) (gpointer user_data);

/* library and state are used to move installed games off GE-Proton
 * versions removed from here. */
void gog_show_preferences_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                                   GogState *state,
                                   GogLibraryPathChangedFunc on_library_path_changed,
                                   GogRefetchCategoriesFunc on_refetch_categories,
                                   gpointer user_data);

G_END_DECLS

#endif
