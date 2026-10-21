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
  GOG_SYNC_PART_PATCH = 1 << 3, /* patches, the game's and its DLC's; only listed with
                                 * gog_sync_get_include_patches() */
} GogSyncPart;

#define GOG_SYNC_PART_ALL                                                                    \
  (GOG_SYNC_PART_GAME | GOG_SYNC_PART_DLC | GOG_SYNC_PART_EXTRA | GOG_SYNC_PART_PATCH)

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
  gchar *title;     /* GOG's title, from --list j; NULL if not known */
  gint64 product_id; /* GOG's product id, from --list j; 0 if not known */
  gchar *changelog;  /* GOG's changelog HTML for the version on GOG, from --list j;
                      * NULL if none */
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
/* lgogdownloader's --limit-rate for downloads, in kB/s; 0 = no limit (the
 * default). */
guint gog_sync_get_rate_limit (void);
void  gog_sync_set_rate_limit (guint kilobytes_per_second);
/* Whether checks and downloads also take the games hidden on the GOG
 * account page (--include-hidden-products); off by default. */
gboolean gog_sync_get_include_hidden (void);
void     gog_sync_set_include_hidden (gboolean include);
/* Whether checks, downloads and file checks take GOG's patches too, instead
 * of leaving them out (--exclude patches); off by default. */
gboolean gog_sync_get_include_patches (void);
void     gog_sync_set_include_patches (gboolean include);
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

/* GOG's notifications at the last full check (lgogdownloader
 * --notifications), the ones that aren't zero, e.g. "2 new forum replies
 * · 1 unread chat message"; NULL when all were zero or never checked.
 * Transfer full. */
gchar *gog_sync_get_notifications (void);

/* That text from --notifications output ("New forum replies: 2" lines);
 * NULL when all are zero. Exposed for tests. Transfer full. */
gchar *gog_sync_describe_notifications (const char *output);

/* TRUE if lgogdownloader can be run: the AppImage's own copy, or one on
 * PATH. */
gboolean gog_sync_is_available (void);

/* TRUE when the AppImage's own lgogdownloader is used (so there is no
 * lgogdownloader command in a terminal to point people to). */
gboolean gog_sync_is_bundled (void);

/* argv to run lgogdownloader with args (the program, then args, then what
 * the bundled copy needs). Transfer full: free with g_free() only; the
 * strings are args' and static ones. */
gchar **gog_sync_lgog_argv (const char *const *args);

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
 * item's languages_offered, title, product_id and changelog, from `lgogdownloader --list j` output (other
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

/* The same for the games GOG has flagged as updated or new (`--list games
 * --updated` and `--new`), which is much faster than a full check; the
 * other games keep their saved results. GOG's flags stay as they are. */
GogSyncJob *gog_sync_job_new_quick_check (const char *library_dir);

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

/* Checks a downloaded game's files against GOG's checksums (lgogdownloader
 * --repair, patches left out as in the library): a damaged part of a file
 * is downloaded again in place. A file of another size (another version)
 * is reported but left alone, and files not downloaded are skipped. */
GogSyncJob *gog_sync_job_new_repair (const char *library_dir, const char *gamename);

/* After a repair: what it found, readable; NULL before or after other
 * jobs. */
const char *gog_sync_job_get_summary (GogSyncJob *self);

/* That text from --repair output. Exposed for tests. Transfer full. */
gchar *gog_sync_describe_repair (const char *output);

/* ---- Galaxy cloud saves ----
 *
 * lgogdownloader's --galaxy-show-cloud-saves, --galaxy-download-cloud-saves
 * and --galaxy-upload-cloud-saves for a Windows game installed in a Wine or
 * Proton prefix (lgogdownloader has them for Windows builds only). Three
 * things in lgogdownloader 3.19 are worked around:
 *   - a run saves the game's own login over lgogdownloader's GOG Galaxy one,
 *     after which cloud saves fail for every other game: the jobs run it
 *     with a private copy of its login (XDG_CONFIG_HOME), thrown away after;
 *   - its download skips the files that are newer on GOG and fetches the
 *     ones that are newer here: downloads ask for the files to fetch by name
 *     (--cloud-force --cloud-whitelist), worked out from the listing;
 *   - it looks for "Save Games" where Wine has "Saved Games": a link is made.
 * The location of each save comes from GOG, with <?INSTALL?> the game's
 * install folder. */

typedef enum
{
  GOG_CLOUD_SHOW,     /* list the saves on GOG */
  GOG_CLOUD_DOWNLOAD, /* fetch the files asked for, then list */
  GOG_CLOUD_UPLOAD,   /* send the saves that are newer here or only here, then list */
} GogCloudAction;

typedef enum
{
  GOG_CLOUD_FILE_ONLY_ON_GOG,  /* not here */
  GOG_CLOUD_FILE_NOT_NEWER,    /* here, as old as GOG's copy or older */
  GOG_CLOUD_FILE_NEWER_HERE,   /* here, changed since GOG's copy */
  GOG_CLOUD_FILE_LARGER_HERE,  /* here, bigger than GOG's copy (lgogdownloader calls it an
                                * unfinished download; it may be a newer save) */
} GogCloudFileState;

typedef struct
{
  gchar *path; /* GOG's name for it: "<location>/<path under it>" */
  GogCloudFileState state;
} GogCloudFile;

void gog_cloud_file_free (GogCloudFile *file);

/* The user name a prefix has its folders under: Proton's is always
 * "steamuser", Wine's the login name. Transfer none. */
const char *gog_sync_cloud_wine_user (gboolean proton);

/* A cloud-save job for product_id installed in install_dir, with its
 * prefix's root (drive_c's parent) wine_root. paths are the files to fetch
 * for GOG_CLOUD_DOWNLOAD (GogCloudFile.path), NULL otherwise. */
GogSyncJob *gog_sync_job_new_cloud (GogCloudAction action, gint64 product_id,
                                    const char *install_dir, const char *wine_root,
                                    const char *wine_user, const char *const *paths);

/* After a cloud job: what is on GOG, sorted by path (GogCloudFile*, owned by
 * the job); NULL before. */
GPtrArray *gog_sync_job_get_cloud_files (GogSyncJob *self);

/* The first Gen 2 build's id in --galaxy-show-builds output; NULL when
 * there is none. Exposed for tests. Transfer full. */
gchar *gog_sync_cloud_parse_builds (const char *output);

/* The files in --galaxy-show-cloud-saves output ("<path> :: <state>"),
 * sorted by path. Exposed for tests. Element type: GogCloudFile*. */
GPtrArray *gog_sync_cloud_parse_list (const char *output);

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

/* After a job that failed: TRUE when it was because lgogdownloader isn't
 * logged in to GOG. */
gboolean gog_sync_job_needs_login (GogSyncJob *self);

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
