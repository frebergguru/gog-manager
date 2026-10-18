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

/* What a file belongs to, so the dialog can offer each group on its own. */
typedef enum
{
  GOG_SYNC_PART_GAME  = 1 << 0, /* the game's installers and language packs */
  GOG_SYNC_PART_DLC   = 1 << 1, /* DLC installers and language packs */
  GOG_SYNC_PART_EXTRA = 1 << 2, /* extras (manuals, soundtracks...), the game's and its DLC's */
} GogSyncPart;

#define GOG_SYNC_PART_ALL (GOG_SYNC_PART_GAME | GOG_SYNC_PART_DLC | GOG_SYNC_PART_EXTRA)

typedef struct
{
  gchar *gamename;  /* as --status prints it: the game's, or the DLC's */
  gchar *filename;
  gchar *label;     /* GOG's name for it ("Soundtrack (MP3)"), may be NULL */
  gchar *dlc_title; /* the DLC it belongs to, NULL for the game's own */
  GogSyncPart part;
  guint languages;  /* lgogdownloader language bits; 0 if unknown (always wanted) */
  guint64 size;     /* bytes, 0 if unknown */
} GogSyncFile;

typedef struct
{
  gchar *gamename; /* lgogdownloader's name, also the library folder name */
  GogSyncKind kind;
  guint n_files;   /* files missing or differing */
  GPtrArray *files; /* GogSyncFile*, the files counted in n_files */
  guint languages_offered; /* union of the languages of all the game's installers */
} GogSyncItem;

void gog_sync_item_free (GogSyncItem *item);

/* lgogdownloader's languages; entry i is language bit 1 << i. */
typedef struct
{
  const char *code; /* for --language */
  const char *name;
} GogSyncLanguage;

const GogSyncLanguage *gog_sync_get_languages (guint *n_languages);

/* "English, Norwegian"; "All languages" for 0. Transfer full. */
gchar *gog_sync_describe_languages (guint languages);

/* TRUE if file is downloaded with languages chosen (0 = all): extras are
 * not language-specific, and files of unknown language always count. */
gboolean gog_sync_file_wanted (const GogSyncFile *file, guint languages);

/* The languages to download for item, as far as it has them (0 = all):
 * the game's own choice, or else the default (see below). */
guint gog_sync_item_languages (GogSyncItem *item);

/* The default languages for a game offered in `offered` (0 = all): its
 * original language when that default is on, plus the default extra
 * languages. Falls back to English, or else all, when none of those is
 * offered (e.g. the original language isn't known). */
guint gog_sync_default_languages_for (const char *gamename, guint offered);

/* "Norwegian (made in Norway)" for the menus. Transfer full. */
gchar *gog_sync_describe_default_for (const char *gamename, guint offered);

/* TRUE if item has a file in the languages chosen for it (an item saved
 * before files were listed always has). */
gboolean gog_sync_item_has_wanted_files (GogSyncItem *item);

/* Download choices remembered between runs: how many files download at
 * the same time (1-8), and each game's languages: GOG_SYNC_LANGUAGES_DEFAULT
 * (nothing chosen for it), 0 for all, or the languages chosen. */
#define GOG_SYNC_LANGUAGES_DEFAULT G_MAXUINT
guint gog_sync_get_threads (void);
void  gog_sync_set_threads (guint threads);
guint gog_sync_get_game_languages (const char *gamename);
void  gog_sync_set_game_languages (const char *gamename, guint languages);

/* The default for games with no languages of their own: whether to take
 * each game's original language (on unless turned off), and which other
 * languages to always take (none unless chosen). */
gboolean gog_sync_get_default_original (void);
void     gog_sync_set_default_original (gboolean original);
guint    gog_sync_get_default_languages (void);
void     gog_sync_set_default_languages (guint languages);

/* name with its version numbers blanked out, to tell an old version of a
 * file from a new one ("setup_x_#_(#)-#bin"). Transfer full. */
gchar *gog_sync_version_blind_name (const char *name);

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

/* Fills in each pending file's part, language, size and label, and each
 * item's languages_offered, from `lgogdownloader --list j` output (other
 * lines before the JSON are skipped). Files not found keep part GAME (DLC
 * if their gamename is not the item's), language 0 and size 0. */
void gog_sync_apply_details (GPtrArray *items, const char *list_output);

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

/* The same for gamenames only (quick); the other games keep their saved
 * results. */
GogSyncJob *gog_sync_job_new_check_games (const char *library_dir, const char *const *gamenames);

typedef struct
{
  const char *gamename;
  GogSyncPart parts;  /* what to download */
  guint languages;    /* 0 = all */
} GogSyncRequest;

/* Downloads what requests ask for with the options the library was made
 * with, threads files at a time. When every download succeeded,
 * --check-orphans --delete-orphans runs over those games with every
 * language and part, which deletes the old versions (and only them). */
GogSyncJob *gog_sync_job_new_download (const char *library_dir, const GogSyncRequest *requests,
                                       guint n_requests, guint threads);

/* Signals (all emitted on the main-loop thread):
 *   "progress" (gdouble fraction, gchar *message) — fraction -1 when unknown.
 *     While downloading, the fraction is of all the bytes to download.
 *   "file-progress" (gint slot, gchar *filename, gdouble fraction, gchar *detail)
 *     — one download thread's file; filename NULL when the slot is idle.
 *     detail is lgogdownloader's "12.3/100.0MB @ 2.1MB/s ETA: 40s".
 *   "log-line" (gchar *line)
 *   "finished" (gboolean success, gchar *error_message) — NULL on success. */
void gog_sync_job_start  (GogSyncJob *self);
void gog_sync_job_cancel (GogSyncJob *self);

/* After a successful check. Element type: GogSyncItem*, owned by the job. */
GPtrArray *gog_sync_job_get_items (GogSyncJob *self);

typedef struct
{
  gchar *gamename; /* the listed game it belongs to */
  gchar *filename;
  gchar *reason;   /* readable, e.g. "not found on GOG's server (HTTP 404)" */
} GogSyncFailure;

void gog_sync_failure_free (GogSyncFailure *failure);

/* After a download: the files that failed (HTTP and network errors) or
 * that lgogdownloader skipped. They stay in the saved results, and their
 * games keep their old versions. Element type: GogSyncFailure*, owned by
 * the job; empty after a cancel. */
GPtrArray *gog_sync_job_get_failures (GogSyncJob *self);

/* Stops listing these files (until a new version renames them) and takes
 * them out of the saved results. */
void gog_sync_ignore_files (GPtrArray *failures);

G_END_DECLS

#endif
