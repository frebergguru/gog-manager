/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_PROTON_FETCH_H
#define GOG_PROTON_FETCH_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Where downloaded GE-Proton builds are extracted:
 * $XDG_DATA_HOME/gog-manager/proton/<tag>/ (each containing a "proton"
 * executable, same layout Heroic/Steam use for compatibilitytools.d).
 * Created on first use. Transfer full. */
gchar *proton_fetch_get_tools_dir (void);

#define GOG_TYPE_PROTON_FETCH_JOB (gog_proton_fetch_job_get_type ())
G_DECLARE_FINAL_TYPE (GogProtonFetchJob, gog_proton_fetch_job, GOG, PROTON_FETCH_JOB, GObject)

/* Downloads and extracts the latest GE-Proton release from
 * GloriousEggroll/proton-ge-custom on GitHub (via `curl` + `tar`, mirroring
 * how the rest of the app shells out rather than adding an HTTP client
 * dependency). No-op success if that version is already present.
 *
 * Signals (all emitted on the main-loop thread):
 *   "progress" (gdouble fraction, gchar *message) — fraction is -1 while
 *      checking GitHub / extracting (no byte-accurate progress available
 *      for those steps), and tracks downloaded/total bytes during the
 *      download itself.
 *   "log-line" (gchar *line) — diagnostic lines for a log view.
 *   "finished" (gboolean success, gchar *version_or_error) — on success,
 *      the GE-Proton tag that is now installed (e.g. "GE-Proton9-27");
 *      on failure, a human-readable error message.
 */
GogProtonFetchJob *gog_proton_fetch_job_new (void);

void gog_proton_fetch_job_start  (GogProtonFetchJob *self);
void gog_proton_fetch_job_cancel (GogProtonFetchJob *self);

G_END_DECLS

#endif
