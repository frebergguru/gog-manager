#ifndef GOG_INSTALL_DIALOG_H
#define GOG_INSTALL_DIALOG_H

#include <adwaita.h>
#include "../game.h"
#include "../config.h"

G_BEGIN_DECLS

/* Presents a modal dialog to install `installer` (belonging to `game`):
 * lets the user pick a runner (for Windows installers) and an install
 * directory, then runs a GogInstallJob with a live progress bar/log.
 * On success, updates game's install-state properties and persists them
 * via `state` (gog_state_save()). Calls on_done(game, success, user_data)
 * once the dialog is closed (whether installed or cancelled) so the
 * caller can refresh its view. */
typedef void (*GogInstallDoneFunc) (GogGame *game, gboolean success, gpointer user_data);

void gog_show_install_dialog (GtkWindow *parent, GogGame *game, GogInstallerInfo *installer,
                               GogConfig *config, GogState *state, GogInstallDoneFunc on_done,
                               gpointer user_data);

G_END_DECLS

#endif
