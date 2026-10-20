/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gamesdb.h"
#include "config.h"
#include "fetch.h"
#include "json_util.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#define CACHE_FILE "gog-manager-gamesdb.json"
#define IMAGES_DIR "gamesdb-images"
#define API_URL "https://gamesdb.gog.com/platforms/gog/external_releases/%" G_GINT64_FORMAT
/* Image sizes GOG Galaxy uses: 342x482 box art, 1600x523 background. */
#define COVER_FORMAT "_glx_vertical_cover"
#define BACKGROUND_FORMAT "_glx_bg_top_padding_7"
/* Games or images per curl run. */
#define BATCH 12
/* Games gamesdb didn't know are asked about again after this long. */
#define RETRY_SECONDS (30 * 24 * 3600)

static G_LOCK_DEFINE (cache);

void
gog_gamesdb_info_free (GogGamesdbInfo *info)
{
  if (!info)
    return;
  g_free (info->summary);
  g_free (info->series);
  g_strfreev (info->developers);
  g_strfreev (info->publishers);
  g_strfreev (info->themes);
  g_strfreev (info->game_modes);
  g_free (info->cover_url);
  g_free (info->background_url);
  g_free (info->cover_path);
  g_free (info->background_path);
  g_free (info);
}

/* ---- reading gamesdb's answer ---- */

/* gamesdb gives names either as a string or per locale: {"*": ..,
 * "en-US": ..}. English first, as the rest of the app is. */
static const char *
localized (json_object *obj, const char *key)
{
  json_object *value = NULL;
  if (!obj || !json_object_object_get_ex (obj, key, &value) || !value)
    return NULL;
  const char *text = NULL;
  if (json_object_is_type (value, json_type_string))
    text = json_object_get_string (value);
  else if (json_object_is_type (value, json_type_object))
    {
      text = ju_get_string (value, "en-US", NULL);
      if (!text || !*text)
        text = ju_get_string (value, "*", NULL);
    }
  return text && *text ? text : NULL;
}

/* The names in a list of {"name": ...} objects, without repeats. */
static GStrv
names (json_object *game, const char *key)
{
  GStrvBuilder *b = g_strv_builder_new ();
  GPtrArray *seen = g_ptr_array_new ();
  json_object *list = ju_get_array (game, key);
  for (size_t i = 0; list && i < json_object_array_length (list); i++)
    {
      const char *name = localized (json_object_array_get_idx (list, i), "name");
      if (!name || g_ptr_array_find_with_equal_func (seen, name, g_str_equal, NULL))
        continue;
      g_ptr_array_add (seen, (gpointer) name);
      g_strv_builder_add (b, name);
    }
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_ptr_array_unref (seen);
  return out;
}

/* A JPEG of the image in the given size, from its url_format, e.g.
 * "https://images.gog.com/<hash>{formatter}.{ext}?namespace=gamesdb". */
static gchar *
image_url (json_object *game, const char *key, const char *format)
{
  const char *url_format = ju_get_string (ju_get_obj (game, key), "url_format", NULL);
  if (!url_format || !g_str_has_prefix (url_format, "https://") ||
      !strstr (url_format, "{formatter}") || !strstr (url_format, "{ext}"))
    return NULL;
  GString *url = g_string_new (url_format);
  g_string_replace (url, "{formatter}", format, 1);
  g_string_replace (url, "{ext}", "jpg", 1);
  return g_string_free (url, FALSE);
}

GogGamesdbInfo *
gog_gamesdb_parse (const char *json)
{
  json_object *root = json ? json_tokener_parse (json) : NULL;
  json_object *game = ju_get_obj (root, "game");
  if (!root || !json_object_is_type (root, json_type_object) || !game)
    {
      if (root)
        json_object_put (root);
      return NULL;
    }

  GogGamesdbInfo *info = g_new0 (GogGamesdbInfo, 1);
  /* The release's own summary is about GOG's version; the game's is the
   * general one. */
  const char *summary = localized (root, "summary");
  info->summary = g_strdup (summary ? summary : localized (game, "summary"));
  if (info->summary)
    g_strstrip (info->summary);
  const char *series = localized (ju_get_obj (root, "series"), "name");
  info->series = g_strdup (series ? series : localized (ju_get_obj (game, "series"), "name"));
  info->developers = names (game, "developers");
  info->publishers = names (game, "publishers");
  info->themes = names (game, "themes");
  info->game_modes = names (game, "game_modes");
  info->cover_url = image_url (game, "vertical_cover", COVER_FORMAT);
  info->background_url = image_url (game, "background", BACKGROUND_FORMAT);
  json_object_put (root);
  return info;
}

/* ---- the cache ---- */

static json_object *
load_cache_json (void)
{
  gchar *path = gog_config_app_file (CACHE_FILE);
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (!root || !json_object_is_type (root, json_type_object))
    {
      if (root)
        json_object_put (root);
      root = json_object_new_object ();
    }
  return root;
}

static void
save_cache (json_object *root)
{
  gchar *path = gog_config_app_file (CACHE_FILE);
  GError *error = NULL;
  if (!g_file_set_contents (path, json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY),
                            -1, &error))
    {
      g_warning ("Could not save gamesdb details: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
}

static gchar *
images_dir (void)
{
  return gog_config_app_file (IMAGES_DIR);
}

/* The local file for an image URL: its own name, which changes when GOG
 * changes the art. NULL for a name that isn't a plain file name. */
static gchar *
image_path (const char *dir, const char *url)
{
  if (!url)
    return NULL;
  const char *slash = strrchr (url, '/');
  gchar *name = g_strndup (slash ? slash + 1 : url, strcspn (slash ? slash + 1 : url, "?#"));
  gboolean plain = *name && name[0] != '.';
  for (const char *c = name; plain && *c; c++)
    plain = g_ascii_isalnum (*c) || *c == '_' || *c == '-' || *c == '.';
  gchar *path = plain ? g_build_filename (dir, name, NULL) : NULL;
  g_free (name);
  return path;
}

static json_object *
strv_to_json (GStrv strv)
{
  json_object *list = json_object_new_array ();
  for (GStrv s = strv; s && *s; s++)
    json_object_array_add (list, json_object_new_string (*s));
  return list;
}

static GStrv
json_to_strv (json_object *entry, const char *key)
{
  GStrvBuilder *b = g_strv_builder_new ();
  json_object *list = ju_get_array (entry, key);
  for (size_t i = 0; list && i < json_object_array_length (list); i++)
    {
      json_object *item = json_object_array_get_idx (list, i);
      if (json_object_is_type (item, json_type_string))
        g_strv_builder_add (b, json_object_get_string (item));
    }
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return out;
}

static void
add_string (json_object *entry, const char *key, const char *value)
{
  if (value)
    json_object_object_add (entry, key, json_object_new_string (value));
}

/* info NULL: gamesdb doesn't know the game. */
static void
record (json_object *cache, gint64 id, GogGamesdbInfo *info, gint64 now)
{
  json_object *entry = json_object_new_object ();
  if (info)
    {
      add_string (entry, "summary", info->summary);
      add_string (entry, "series", info->series);
      json_object_object_add (entry, "developers", strv_to_json (info->developers));
      json_object_object_add (entry, "publishers", strv_to_json (info->publishers));
      json_object_object_add (entry, "themes", strv_to_json (info->themes));
      json_object_object_add (entry, "game_modes", strv_to_json (info->game_modes));
      add_string (entry, "cover_url", info->cover_url);
      add_string (entry, "background_url", info->background_url);
    }
  else
    json_object_object_add (entry, "missing", json_object_new_boolean (TRUE));
  json_object_object_add (entry, "checked", json_object_new_int64 (now));
  gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, id);
  json_object_object_add (cache, key, entry);
  g_free (key);
}

static gchar *
existing_image (const char *dir, const char *url)
{
  gchar *path = image_path (dir, url);
  if (path && !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    g_clear_pointer (&path, g_free);
  return path;
}

GHashTable *
gog_gamesdb_load (void)
{
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) gog_gamesdb_info_free);
  gchar *dir = images_dir ();
  G_LOCK (cache);
  json_object *root = load_cache_json ();
  G_UNLOCK (cache);
  json_object_object_foreach (root, key, entry)
    {
      if (ju_get_bool (entry, "missing", FALSE))
        continue;
      GogGamesdbInfo *info = g_new0 (GogGamesdbInfo, 1);
      info->summary = g_strdup (ju_get_string (entry, "summary", NULL));
      info->series = g_strdup (ju_get_string (entry, "series", NULL));
      info->developers = json_to_strv (entry, "developers");
      info->publishers = json_to_strv (entry, "publishers");
      info->themes = json_to_strv (entry, "themes");
      info->game_modes = json_to_strv (entry, "game_modes");
      info->cover_url = g_strdup (ju_get_string (entry, "cover_url", NULL));
      info->background_url = g_strdup (ju_get_string (entry, "background_url", NULL));
      info->cover_path = existing_image (dir, info->cover_url);
      info->background_path = existing_image (dir, info->background_url);
      g_hash_table_insert (table, g_strdup (key), info);
    }
  json_object_put (root);
  g_free (dir);
  return table;
}

void
gog_gamesdb_forget (void)
{
  G_LOCK (cache);
  gchar *path = gog_config_app_file (CACHE_FILE);
  if (g_remove (path) != 0 && errno != ENOENT)
    g_warning ("Could not remove %s: %s", path, g_strerror (errno));
  g_free (path);
  G_UNLOCK (cache);
}

/* ---- looking games up ---- */

/* Fetches up to BATCH games in one curl run and records the answers. A game
 * that could not be fetched (no connection, GOG busy) is left for next
 * time; the batch fails only when none of its games could be fetched. */
static gboolean
lookup_batch (const gint64 *ids, guint n, json_object *cache, GError **error)
{
  gchar *dir = g_dir_make_tmp ("gog-manager-gamesdb-XXXXXX", error);
  if (!dir)
    return FALSE;

  gchar **urls = g_new0 (gchar *, n + 1);
  gchar **dests = g_new0 (gchar *, n + 1);
  int *codes = g_new0 (int, n);
  for (guint i = 0; i < n; i++)
    {
      urls[i] = g_strdup_printf (API_URL, ids[i]);
      dests[i] = g_strdup_printf ("%s/%" G_GINT64_FORMAT ".json", dir, ids[i]);
    }
  gchar *curl_error = NULL;
  gboolean ok = gog_fetch_files ((const char *const *) urls, (const char *const *) dests, n, codes,
                                 &curl_error, error);

  guint fetched = 0;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  for (guint i = 0; ok && i < n; i++)
    {
      if (codes[i] == 200)
        {
          gchar *body = NULL;
          GogGamesdbInfo *info = g_file_get_contents (dests[i], &body, NULL, NULL)
                                     ? gog_gamesdb_parse (body)
                                     : NULL;
          g_free (body);
          if (info)
            {
              record (cache, ids[i], info, now);
              fetched++;
            }
          gog_gamesdb_info_free (info);
        }
      else if (codes[i] == 404)
        {
          record (cache, ids[i], NULL, now);
          fetched++;
        }
    }
  gog_fetch_remove_dir (dir);
  g_free (dir);
  g_strfreev (urls);
  g_strfreev (dests);
  g_free (codes);

  if (ok && fetched == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not fetch details from GOG's games database: %s",
                   curl_error ? curl_error : "no answer");
      ok = FALSE;
    }
  g_free (curl_error);
  return ok;
}

static void
want_image (GPtrArray *urls, GPtrArray *paths, const char *dir, const char *url)
{
  gchar *path = image_path (dir, url);
  if (!path || g_file_test (path, G_FILE_TEST_EXISTS) ||
      g_ptr_array_find_with_equal_func (paths, path, g_str_equal, NULL))
    {
      g_free (path);
      return;
    }
  g_ptr_array_add (urls, g_strdup (url));
  g_ptr_array_add (paths, path);
}

/* Downloads the covers and backgrounds of the given games that aren't on
 * disk yet. One that doesn't come is tried again on the next lookup. */
static gboolean
fetch_images (const gint64 *ids, guint n_ids, GCancellable *cancellable,
              GogGamesdbProgress progress, gpointer user_data, GError **error)
{
  gchar *dir = images_dir ();
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "Could not create %s",
                   dir);
      g_free (dir);
      return FALSE;
    }

  G_LOCK (cache);
  json_object *cache = load_cache_json ();
  G_UNLOCK (cache);
  GPtrArray *urls = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < n_ids; i++)
    {
      gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, ids[i]);
      json_object *entry = ju_get_obj (cache, key);
      g_free (key);
      want_image (urls, paths, dir, ju_get_string (entry, "cover_url", NULL));
      want_image (urls, paths, dir, ju_get_string (entry, "background_url", NULL));
    }
  json_object_put (cache);

  gboolean ok = TRUE;
  for (guint i = 0; ok && i < urls->len && !g_cancellable_is_cancelled (cancellable); i += BATCH)
    {
      guint n = MIN (BATCH, urls->len - i);
      /* Into .part files first: a download cut off half way must not
       * look like a finished image. */
      gchar **parts = g_new0 (gchar *, n + 1);
      for (guint j = 0; j < n; j++)
        parts[j] = g_strconcat (g_ptr_array_index (paths, i + j), ".part", NULL);
      int *codes = g_new0 (int, n);
      ok = gog_fetch_files ((const char *const *) &g_ptr_array_index (urls, i),
                            (const char *const *) parts, n, codes, NULL, error);
      for (guint j = 0; j < n; j++)
        {
          if (!(ok && codes[j] == 200 &&
                g_rename (parts[j], g_ptr_array_index (paths, i + j)) == 0))
            g_remove (parts[j]);
        }
      g_strfreev (parts);
      g_free (codes);
      if (progress)
        progress (i + n, urls->len, TRUE, user_data);
    }
  g_ptr_array_unref (urls);
  g_ptr_array_unref (paths);
  g_free (dir);
  return ok;
}

/* Removes images no game in the cache uses any more (old art, or the
 * cache was emptied and the game is gone). */
static void
prune_images (void)
{
  gchar *dir = images_dir ();
  G_LOCK (cache);
  json_object *cache = load_cache_json ();
  G_UNLOCK (cache);
  GHashTable *used = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  json_object_object_foreach (cache, key, entry)
    {
      (void) key;
      const char *keys[] = { "cover_url", "background_url" };
      for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
        {
          gchar *path = image_path (dir, ju_get_string (entry, keys[i], NULL));
          if (path)
            g_hash_table_add (used, path);
        }
    }
  json_object_put (cache);

  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      gchar *path = g_build_filename (dir, name, NULL);
      if (!g_hash_table_contains (used, path))
        g_remove (path);
      g_free (path);
    }
  if (d)
    g_dir_close (d);
  g_hash_table_unref (used);
  g_free (dir);
}

gboolean
gog_gamesdb_lookup (const gint64 *ids, guint n_ids, GCancellable *cancellable,
                    GogGamesdbProgress progress, gpointer user_data, guint *looked_up,
                    GError **error)
{
  /* The lock is held only while the file is read or written, never across
   * the network: the UI thread takes it in gog_gamesdb_load(). */
  G_LOCK (cache);
  json_object *cache = load_cache_json ();
  G_UNLOCK (cache);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  GArray *todo = g_array_new (FALSE, FALSE, sizeof (gint64));
  for (guint i = 0; i < n_ids; i++)
    {
      if (ids[i] <= 0)
        continue;
      gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, ids[i]);
      json_object *entry = ju_get_obj (cache, key);
      g_free (key);
      gboolean known = entry && !ju_get_bool (entry, "missing", FALSE);
      if (entry && (known || now - ju_get_int64 (entry, "checked", 0) < RETRY_SECONDS))
        continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < todo->len && !dup; j++)
        dup = g_array_index (todo, gint64, j) == ids[i];
      if (!dup)
        g_array_append_val (todo, ids[i]);
    }
  json_object_put (cache);
  if (looked_up)
    *looked_up = todo->len;

  gboolean ok = TRUE;
  for (guint i = 0; ok && i < todo->len && !g_cancellable_is_cancelled (cancellable); i += BATCH)
    {
      guint n = MIN (BATCH, todo->len - i);
      json_object *answers = json_object_new_object ();
      ok = lookup_batch (&g_array_index (todo, gint64, i), n, answers, error);
      /* Merged into the file as it is now and saved as it goes, so closing
       * the app half way loses nothing. */
      G_LOCK (cache);
      json_object *current = load_cache_json ();
      json_object_object_foreach (answers, key, entry)
        json_object_object_add (current, key, json_object_get (entry));
      save_cache (current);
      json_object_put (current);
      G_UNLOCK (cache);
      json_object_put (answers);
      if (progress)
        progress (i + n, todo->len, FALSE, user_data);
    }
  g_array_unref (todo);

  /* Images also for games looked up before, whose files went missing. */
  if (ok && !g_cancellable_is_cancelled (cancellable))
    ok = fetch_images (ids, n_ids, cancellable, progress, user_data, error);
  if (ok && !g_cancellable_is_cancelled (cancellable))
    prune_images ();
  return ok;
}
