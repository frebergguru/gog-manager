#ifndef GOG_INSTALLER_ENGINE_H
#define GOG_INSTALLER_ENGINE_H

#include <gio/gio.h>
#include "game.h"
#include "runner.h"

G_BEGIN_DECLS

#define GOG_TYPE_INSTALL_JOB (gog_install_job_get_type ())
G_DECLARE_FINAL_TYPE (GogInstallJob, gog_install_job, GOG, INSTALL_JOB, GObject)

/* Installs `installer` (must belong to `game`) into
 * "<install_root>/<game slug>" (or, when installing a DLC resolved to a
 * base game, into the base game's existing install directory instead).
 * `runner` is required for Windows installers (Wine or Proton) and ignored
 * for Linux-native ones; a copy is taken, ownership stays with the caller.
 *
 * Signals (all emitted on the main-loop thread):
 *   "progress" (gdouble fraction, gchar *message) — fraction is -1 for
 *      indeterminate stretches (e.g. unzip extraction gives no percentage).
 *   "log-line" (gchar *line) — raw subprocess output, for a live log view.
 *   "finished" (gboolean success, gchar *error_message) — error_message is
 *      NULL on success.
 */
GogInstallJob *gog_install_job_new (GogGame *game, GogInstallerInfo *installer,
                                     const char *install_root, GogRunnerInfo *runner);

void gog_install_job_start  (GogInstallJob *self);
void gog_install_job_cancel (GogInstallJob *self);

const char *gog_install_job_get_install_dir (GogInstallJob *self);

/* Removes an installed game's files (and Wine/Proton prefix, if any).
 * Synchronous — callers should run this off the main thread for large
 * installs, or accept a brief UI stall for small ones. */
gboolean gog_uninstall_game (GogGame *game, GError **error);

G_END_DECLS

#endif
