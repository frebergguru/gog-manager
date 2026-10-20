/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "categories.h"
#include "config.h"
#include "json_util.h"
#include <glib/gstdio.h>
#include <string.h>

#define CACHE_FILE "gog-manager-categories.json"
#define API_URL "https://api.gog.com/v2/games/%" G_GINT64_FORMAT "?locale=en-US"
/* Games per curl run; it fetches PARALLEL of them at a time. */
#define BATCH 12
#define PARALLEL "4"
/* Games GOG didn't know are asked about again after this long. */
#define RETRY_SECONDS (30 * 24 * 3600)

static G_LOCK_DEFINE (cache);

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
      g_warning ("Could not save categories: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
}

static GStrv
entry_categories (json_object *entry)
{
  GStrvBuilder *b = g_strv_builder_new ();
  json_object *list = ju_get_array (entry, "categories");
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

GHashTable *
gog_categories_load (void)
{
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) g_strfreev);
  G_LOCK (cache);
  json_object *root = load_cache_json ();
  json_object_object_foreach (root, key, entry)
    g_hash_table_insert (table, g_strdup (key), entry_categories (entry));
  json_object_put (root);
  G_UNLOCK (cache);
  return table;
}

GStrv
gog_categories_parse (const char *json)
{
  json_object *root = json ? json_tokener_parse (json) : NULL;
  if (!root || !json_object_is_type (root, json_type_object))
    {
      if (root)
        json_object_put (root);
      return NULL;
    }
  GStrvBuilder *b = g_strv_builder_new ();
  GPtrArray *seen = g_ptr_array_new ();
  json_object *tags = ju_get_array (ju_get_obj (root, "_embedded"), "tags");
  for (size_t i = 0; tags && i < json_object_array_length (tags); i++)
    {
      const char *name = ju_get_string (json_object_array_get_idx (tags, i), "name", NULL);
      if (!name || !*name || g_ptr_array_find_with_equal_func (seen, name, g_str_equal, NULL))
        continue;
      g_ptr_array_add (seen, (gpointer) name);
      g_strv_builder_add (b, name);
    }
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_ptr_array_unref (seen);
  json_object_put (root);
  return out;
}

static void
record (json_object *cache, gint64 id, GStrv categories, gint64 now)
{
  json_object *entry = json_object_new_object ();
  json_object *list = json_object_new_array ();
  for (GStrv c = categories; c && *c; c++)
    json_object_array_add (list, json_object_new_string (*c));
  json_object_object_add (entry, "categories", list);
  json_object_object_add (entry, "checked", json_object_new_int64 (now));
  gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, id);
  json_object_object_add (cache, key, entry);
  g_free (key);
}

static void
remove_dir (const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      gchar *path = g_build_filename (dir, name, NULL);
      g_remove (path);
      g_free (path);
    }
  if (d)
    g_dir_close (d);
  g_rmdir (dir);
}

/* Fetches up to BATCH games in one curl run and records the answers. A game
 * that could not be fetched (no connection, GOG busy) is left for next
 * time; the batch fails only when none of its games could be fetched. */
static gboolean
lookup_batch (const gint64 *ids, guint n, json_object *cache, GError **error)
{
  gchar *dir = g_dir_make_tmp ("gog-manager-categories-XXXXXX", error);
  if (!dir)
    return FALSE;

  /* A plain, generic User-Agent: nothing that identifies the user. One line
   * per game on stdout: "<HTTP status> <file>"; 000 when nothing came back. */
  static const char *const base_argv[] = { "curl", "-sS", "--max-time", "30", "-A", "gog-manager",
                                           "--parallel", "--parallel-max", PARALLEL, "-w",
                                           "%{http_code} %{filename_effective}\\n" };
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (base_argv); i++)
    g_ptr_array_add (argv, g_strdup (base_argv[i]));
  for (guint i = 0; i < n; i++)
    {
      g_ptr_array_add (argv, g_strdup ("-o"));
      g_ptr_array_add (argv, g_strdup_printf ("%s/%" G_GINT64_FORMAT ".json", dir, ids[i]));
      g_ptr_array_add (argv, g_strdup_printf (API_URL, ids[i]));
    }
  g_ptr_array_add (argv, NULL);

  gchar *out = NULL;
  gchar *err = NULL;
  gboolean ok = g_spawn_sync (NULL, (gchar **) argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                              &out, &err, NULL, error);
  g_ptr_array_unref (argv);

  guint fetched = 0;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gchar **lines = g_strsplit (ok && out ? out : "", "\n", -1);
  for (gchar **l = lines; *l; l++)
    {
      int code = 0;
      const char *space = strchr (*l, ' ');
      if (!space || sscanf (*l, "%d", &code) != 1)
        continue;
      gchar *base = g_path_get_basename (space + 1);
      gint64 id = g_ascii_strtoll (base, NULL, 10);
      g_free (base);
      if (id <= 0)
        continue;
      if (code == 200)
        {
          gchar *body = NULL;
          GStrv categories = g_file_get_contents (space + 1, &body, NULL, NULL)
                                 ? gog_categories_parse (body)
                                 : NULL;
          g_free (body);
          if (categories)
            {
              record (cache, id, categories, now);
              fetched++;
            }
          g_strfreev (categories);
        }
      else if (code == 404)
        {
          /* No longer sold, or never a product of its own: GOG has none. */
          record (cache, id, NULL, now);
          fetched++;
        }
    }
  g_strfreev (lines);
  remove_dir (dir);
  g_free (dir);

  if (ok && fetched == 0)
    {
      /* curl says the same for every game; once is enough. */
      gchar *nl = err ? strchr (g_strstrip (err), '\n') : NULL;
      if (nl)
        *nl = '\0';
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not fetch categories from GOG: %s",
                   err && *err ? err : "no answer");
      ok = FALSE;
    }
  g_free (out);
  g_free (err);
  return ok;
}

gboolean
gog_categories_lookup (const gint64 *ids, guint n_ids, GCancellable *cancellable,
                       GogCategoriesProgress progress, gpointer user_data, guint *looked_up,
                       GError **error)
{
  G_LOCK (cache);
  json_object *cache = load_cache_json ();
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  GArray *todo = g_array_new (FALSE, FALSE, sizeof (gint64));
  for (guint i = 0; i < n_ids; i++)
    {
      if (ids[i] <= 0)
        continue;
      gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, ids[i]);
      json_object *entry = ju_get_obj (cache, key);
      g_free (key);
      json_object *list = ju_get_array (entry, "categories");
      gboolean known = list && json_object_array_length (list) > 0;
      if (entry && (known || now - ju_get_int64 (entry, "checked", 0) < RETRY_SECONDS))
        continue;
      gboolean dup = FALSE;
      for (guint j = 0; j < todo->len && !dup; j++)
        dup = g_array_index (todo, gint64, j) == ids[i];
      if (!dup)
        g_array_append_val (todo, ids[i]);
    }
  if (looked_up)
    *looked_up = todo->len;

  gboolean ok = TRUE;
  for (guint i = 0; ok && i < todo->len && !g_cancellable_is_cancelled (cancellable); i += BATCH)
    {
      guint n = MIN (BATCH, todo->len - i);
      ok = lookup_batch (&g_array_index (todo, gint64, i), n, cache, error);
      /* Saved as it goes, so closing the app half way loses nothing. */
      save_cache (cache);
      if (progress)
        progress (i + n, todo->len, user_data);
    }
  g_array_unref (todo);
  json_object_put (cache);
  G_UNLOCK (cache);
  return ok;
}
