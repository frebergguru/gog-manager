/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_CLOUD_SAVES_H
#define GOG_CLOUD_SAVES_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* GOG Galaxy's cloud saves for a Windows game installed in a Wine or Proton
 * prefix, the way GOG Galaxy itself does them:
 *   1. the game's newest Galaxy (generation 2) build on content-system.gog.com
 *      and its manifest give the game's client id and secret;
 *   2. lgogdownloader's GOG Galaxy login (its galaxy_tokens.json, only read)
 *      is exchanged at auth.gog.com for a login of the game's client,
 *      without a new session, as Galaxy does;
 *   3. remote-config.gog.com says where the game keeps its saves
 *      ("<?APPLICATION_DATA_ROAMING?>\\StardewValley"), found here in the
 *      prefix;
 *   4. cloudstorage.gog.com/v1/<user>/<client> lists, sends and fetches them.
 * Blocking: call these from a worker thread. */

#define GOG_CLOUD_ERROR (gog_cloud_error_quark ())
GQuark gog_cloud_error_quark (void);

typedef enum
{
  GOG_CLOUD_ERROR_NEEDS_LOGIN,   /* no usable GOG Galaxy login: log in again */
  GOG_CLOUD_ERROR_NOT_SUPPORTED, /* GOG keeps no cloud saves for this game */
  GOG_CLOUD_ERROR_FAILED,        /* network, server or file trouble */
} GogCloudError;

typedef enum
{
  GOG_CLOUD_IN_SYNC,      /* the same here and on GOG */
  GOG_CLOUD_ONLY_ON_GOG,
  GOG_CLOUD_ONLY_HERE,
  GOG_CLOUD_NEWER_ON_GOG,
  GOG_CLOUD_NEWER_HERE,
} GogCloudState;

typedef struct
{
  gchar *name;        /* GOG's name for it: "<location>/<path under it>" */
  GogCloudState state;
  gchar *local_path;  /* where it is or goes here; NULL when GOG has it under a
                       * location the game no longer has */
  guint64 local_size;
  gint64 local_mtime; /* unix time; 0 when not here */
  guint64 cloud_size;
  gint64 cloud_mtime; /* 0 when not on GOG */
  gchar *cloud_hash;  /* MD5, hex; may be NULL */
} GogCloudFile;

void gog_cloud_file_free (GogCloudFile *file);

/* The installed game. */
typedef struct
{
  gint64 product_id;
  const char *install_dir;
  const char *wine_root; /* the prefix's root, which has drive_c */
  const char *wine_user; /* the user folder under drive_c/users, e.g. "steamuser";
                          * another one found there is used when it is missing */
} GogCloudGame;

typedef struct _GogCloudSession GogCloudSession;

/* Steps 1 to 3. NULL with error set when it can't be done. */
GogCloudSession *gog_cloud_connect (const GogCloudGame *game, GCancellable *cancellable,
                                    GError **error);
void             gog_cloud_session_free (GogCloudSession *session);

/* Every save, on GOG or here, sorted by name. Element type: GogCloudFile*. */
GPtrArray *gog_cloud_list (GogCloudSession *session, GCancellable *cancellable, GError **error);

/* Called before each file, from the calling thread. */
typedef void (*GogCloudProgress) (guint done, guint total, const char *name, gpointer user_data);

/* Fetches files (GogCloudFile*, from gog_cloud_list()) from GOG. A file
 * here that is replaced is copied to backup_dir first (under its name),
 * and a fetched file gets GOG's time. Stops at the first failure. */
gboolean gog_cloud_download (GogCloudSession *session, GPtrArray *files, const char *backup_dir,
                             GogCloudProgress progress, gpointer user_data,
                             GCancellable *cancellable, GError **error);

/* Sends files to GOG, gzipped like GOG Galaxy's. Afterwards each sent file here gets the time GOG gave
 * it, so that the two compare as the same. */
gboolean gog_cloud_upload (GogCloudSession *session, GPtrArray *files, GogCloudProgress progress,
                           gpointer user_data, GCancellable *cancellable, GError **error);

/* Where replaced saves of product_id are kept, a new folder per download
 * (the newest ten are kept). Transfer full. */
gchar *gog_cloud_backup_dir (gint64 product_id);

/* ---- the parts, exposed for tests ---- */

/* The manifest address of the newest generation 2 build in a builds
 * listing, the default branch's before others. NULL if there is none.
 * Transfer full. */
gchar *gog_cloud_parse_builds (const char *json);

/* A build manifest (zlib-compressed JSON, or plain): the game's client id
 * and secret. FALSE when they aren't there. */
gboolean gog_cloud_parse_manifest (const guint8 *data, gsize len, gchar **client_id,
                                   gchar **client_secret);

/* remote-config's answer: the Windows save locations as name, template,
 * name, template... (GOG's default one when it gives none). NULL when cloud
 * saves are off for the game. Transfer full. */
GStrv gog_cloud_parse_locations (const char *json, const char *client_id);

/* A location template as a folder in the game's prefix: "<?VAR?>" replaced,
 * backslashes made slashes, and each folder that exists in another case
 * found as it is. NULL for a variable it doesn't know. Transfer full. */
gchar *gog_cloud_resolve_location (const char *location, const GogCloudGame *game);

/* cloudstorage's listing as GogCloudFile*s with only the cloud fields set,
 * leaving out names that could reach outside a save folder. */
GPtrArray *gog_cloud_parse_listing (const char *json);

/* TRUE for a name that stays inside its location: no empty, "." or ".."
 * parts, not absolute. */
gboolean gog_cloud_name_is_safe (const char *name);

/* The state of a save known on both sides: the same when the checksums
 * match (a file sent unpacked), else by time. local_md5 may be NULL. */
GogCloudState gog_cloud_compare (gint64 local_mtime, guint64 local_size, const char *local_md5,
                                 gint64 cloud_mtime, guint64 cloud_size, const char *cloud_hash);

G_END_DECLS

#endif
