/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_PROTON_MANAGE_DIALOG_H
#define GOG_PROTON_MANAGE_DIALOG_H

#include <adwaita.h>
#include "../config.h"
#include "../library.h"

G_BEGIN_DECLS

/* Called once the dialog is closed. changed is TRUE if any GE-Proton build
 * was removed (so runner lists should be refreshed). */
typedef void (*GogProtonManageDoneFunc) (gboolean changed, gpointer user_data);

/* Presents a dialog listing the GE-Proton builds downloaded by this app,
 * where the user can remove chosen versions or every version except the
 * newest. Installed games and the default runner that pointed at a removed
 * build are moved to the newest remaining build (and saved to state /
 * config). */
void gog_show_proton_manage_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                                     GogState *state, GogProtonManageDoneFunc on_done,
                                     gpointer user_data);

G_END_DECLS

#endif
