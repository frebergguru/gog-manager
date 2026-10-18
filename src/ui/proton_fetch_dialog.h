/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_PROTON_FETCH_DIALOG_H
#define GOG_PROTON_FETCH_DIALOG_H

#include <adwaita.h>
#include "../config.h"
#include "../library.h"

G_BEGIN_DECLS

/* Called once the dialog is closed. success is FALSE if the user closed it
 * without downloading, or if the download/extract failed; result is the
 * installed GE-Proton tag on success, or NULL otherwise. */
typedef void (*GogProtonFetchDoneFunc) (gboolean success, const char *result, gpointer user_data);

/* Presents a modal dialog that downloads and installs the latest GE-Proton
 * release from GitHub, with a live progress bar and log. Afterwards it
 * offers to remove older downloaded versions that no game in library (and
 * not config's default runner) uses. */
void gog_show_proton_fetch_dialog (GtkWindow *parent, GogConfig *config, GogLibrary *library,
                                    GogProtonFetchDoneFunc on_done, gpointer user_data);

G_END_DECLS

#endif
