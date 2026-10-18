/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_PROTON_FETCH_H
#define GOG_PROTON_FETCH_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Where downloaded GE-Proton builds are extracted:
 * $XDG_DATA_HOME/gog-manager/proton/<tag>[-<arch>]/ (each containing a "proton"
 * executable, same layout Heroic/Steam use for compatibilitytools.d).
 * Created on first use. Transfer full. */
gchar *proton_fetch_get_tools_dir (void);

/* Orders two build directory names (plain strings) by version, newest
 * first. Numbers compare numerically, so
 * "GE-Proton11-10-x86_64" sorts before "GE-Proton11-9-x86_64". */
gint proton_fetch_compare_newest_first (gconstpointer a, gconstpointer b);

/* Directory names of the builds under proton_fetch_get_tools_dir(),
 * newest first. Only builds this app downloaded; Steam/Heroic builds live
 * elsewhere and are not ours to delete. Element type: gchar*, transfer full. */
GPtrArray *proton_fetch_list_installed (void);

/* Deletes the named builds (as returned by proton_fetch_list_installed())
 * from the tools dir on a worker thread. Stops at the first failure. */
void     proton_fetch_remove_async  (const char *const *names, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
gboolean proton_fetch_remove_finish (GAsyncResult *result, GError **error);

/* Disk space used by each named build, measured on a worker thread.
 * finish returns a GArray of guint64, one per name in order, transfer full. */
void    proton_fetch_measure_async  (const char *const *names, GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data);
GArray *proton_fetch_measure_finish (GAsyncResult *result, GError **error);

/* TRUE if the release (tag, plus the directory its archive unpacks to) is
 * already in the tools dir. */
gboolean proton_fetch_is_installed (const char *tag, const char *dir_name);

/* Asks GitHub for the newest GE-Proton release without downloading it.
 * The answer is remembered for ten minutes. On success the finish function
 * sets the tag (e.g. "GE-Proton11-9") and the directory name its archive
 * unpacks to (e.g. "GE-Proton11-9-x86_64"), both transfer full. */
void     proton_fetch_check_latest_async  (GCancellable *cancellable,
                                           GAsyncReadyCallback callback, gpointer user_data);
gboolean proton_fetch_check_latest_finish (GAsyncResult *result, gchar **out_tag,
                                           gchar **out_dir_name, GError **error);

#define GOG_TYPE_PROTON_FETCH_JOB (gog_proton_fetch_job_get_type ())
G_DECLARE_FINAL_TYPE (GogProtonFetchJob, gog_proton_fetch_job, GOG, PROTON_FETCH_JOB, GObject)

/* Downloads, checks (against the published SHA-512 sum, when there is one)
 * and extracts the newest published GE-Proton release from
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
