/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_INSTALLER_ENGINE_H
#define GOG_INSTALLER_ENGINE_H

#include <gio/gio.h>
#include "game.h"
#include "runner.h"

G_BEGIN_DECLS

#define GOG_TYPE_INSTALL_JOB (gog_install_job_get_type ())
G_DECLARE_FINAL_TYPE (GogInstallJob, gog_install_job, GOG, INSTALL_JOB, GObject)

/* Installs `installer` (must belong to `game`) into `install_dir` exactly
 * (or, when installing a DLC resolved to an installed base game, into the
 * base game's existing install directory instead).
 * `runner` is required for Windows installers (Wine or Proton) and ignored
 * for Linux-native ones; a copy is taken, ownership stays with the caller.
 *
 * Signals (all emitted on the main-loop thread):
 *   "progress" (gdouble fraction, gchar *message) — fraction is -1 for
 *      indeterminate stretches (e.g. unzip extraction gives no percentage).
 *   "log-line" (gchar *line) — raw subprocess output, for a live log view.
 *   "finished" (gboolean success, gchar *error_message) — error_message is
 *      NULL on success.
 *   "fonts-missing" (gboolean paused) — Wine has no FreeType, so Windows
 *      programs get no fonts (see runner_freetype_fixes()). When paused is
 *      TRUE the install waits for gog_install_job_resume() or
 *      gog_install_job_cancel(); when FALSE the installer is already
 *      running and carries on.
 */
GogInstallJob *gog_install_job_new (GogGame *game, GogInstallerInfo *installer,
                                     const char *install_dir, GogRunnerInfo *runner);

void gog_install_job_start  (GogInstallJob *self);
void gog_install_job_cancel (GogInstallJob *self);
void gog_install_job_resume (GogInstallJob *self);

const char *gog_install_job_get_install_dir (GogInstallJob *self);

/* Removes an installed game's files (and Wine/Proton prefix, if any). For a
 * DLC that shares its base game's directory only the record is cleared.
 * Synchronous — callers should run this off the main thread for large
 * installs, or accept a brief UI stall for small ones. */
gboolean gog_uninstall_game (GogGame *game, GError **error);

/* Roughly how much space an install takes. GOG doesn't say how big a game
 * is once installed, only its installers, which are compressed: twice
 * their size, plus 1 GiB when a new Wine/Proton prefix is made. */
guint64 gog_install_estimate_bytes (guint64 installer_size, gboolean new_prefix);

/* Free bytes for an unprivileged user on the drive that holds path (or
 * the nearest folder above it that exists, as an install folder usually
 * doesn't yet). FALSE when it can't be told. */
gboolean gog_free_space (const char *path, guint64 *free_bytes);

G_END_DECLS

#endif
