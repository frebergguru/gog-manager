/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_SYNC_H
#define GOG_SYNC_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Checking GOG for new games and new versions, and downloading them, by
 * driving lgogdownloader (which holds the GOG login; run
 * `lgogdownloader --login` once). The library folder is expected to be
 * lgogdownloader's download directory (one "<gamename>" folder per game). */

typedef enum
{
  GOG_SYNC_NEW_GAME, /* owned on GOG, no folder in the library yet */
  GOG_SYNC_UPDATE,   /* folder exists, but files are missing or differ */
} GogSyncKind;

typedef struct
{
  gchar *gamename; /* lgogdownloader's name, also the library folder name */
  GogSyncKind kind;
  guint n_files;   /* files missing or differing */
} GogSyncItem;

void gog_sync_item_free (GogSyncItem *item);

/* TRUE if lgogdownloader is on PATH. */
gboolean gog_sync_is_available (void);

/* Folds lgogdownloader `--status` output into one item per game that has
 * files to fetch. DLC is counted under its game: lgogdownloader downloads it
 * with the game and prints its lines right after the game's, so a name not
 * in base_names (from `--list games`; NULL to treat every name as a game)
 * belongs to the game listed before it. Lines are "<code> <gamename> <file>[ <size>[ <hash>]]";
 * ND (not downloaded) lines carry no size, and file names may contain
 * spaces. Codes ND, FS (size differs) and MD5 count; OK does not. A game
 * is NEW_GAME when library_dir has no folder named after it. Element type: GogSyncItem*, transfer full, sorted by name. */
GPtrArray *gog_sync_parse_status (const char *output, const char *library_dir,
                                  const char *const *base_names);

/* Turns a game name into a readable title for games not in the library yet
 * ("broken_sword_4" -> "Broken Sword 4"). Transfer full. */
gchar *gog_sync_pretty_name (const char *gamename);

/* The last check's results, kept next to the app's state file so the
 * library can mark games with updates. Element type: GogSyncItem*. */
GPtrArray *gog_sync_load_results (gint64 *out_checked_unix);
void       gog_sync_save_results (GPtrArray *items);

#define GOG_TYPE_SYNC_JOB (gog_sync_job_get_type ())
G_DECLARE_FINAL_TYPE (GogSyncJob, gog_sync_job, GOG, SYNC_JOB, GObject)

/* A check: login status, then `lgogdownloader --status --size-only
 * --exclude patches`. On success, gog_sync_job_get_items() has the result
 * (also saved with gog_sync_save_results()). */
GogSyncJob *gog_sync_job_new_check (const char *library_dir);

/* Downloads gamenames with the same options the library was made with,
 * including --check-orphans --delete-orphans, which deletes the old
 * versions once the new files are in. */
GogSyncJob *gog_sync_job_new_download (const char *library_dir, const char *const *gamenames);

/* Signals (all emitted on the main-loop thread):
 *   "progress" (gdouble fraction, gchar *message) — fraction -1 when unknown.
 *   "log-line" (gchar *line)
 *   "finished" (gboolean success, gchar *error_message) — NULL on success. */
void gog_sync_job_start  (GogSyncJob *self);
void gog_sync_job_cancel (GogSyncJob *self);

/* After a successful check. Element type: GogSyncItem*, owned by the job. */
GPtrArray *gog_sync_job_get_items (GogSyncJob *self);

G_END_DECLS

#endif
