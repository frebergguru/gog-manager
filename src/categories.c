/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "categories.h"
#include "config.h"
#include "fetch.h"
#include "json_util.h"
#include "text_util.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

#define CACHE_FILE "gog-manager-categories.json"
#define API_URL "https://api.gog.com/v2/games/%" G_GINT64_FORMAT "?locale=en-US"
/* Games per curl run. */
#define BATCH 12
/* Games GOG didn't know are asked about again after this long. */
#define RETRY_SECONDS (30 * 24 * 3600)
/* What a cache entry holds; older entries are looked up again. 2 added the
 * store details (GogStoreInfo); 3 trims the age ratings' reasons. */
#define CACHE_VERSION 3

static G_LOCK_DEFINE (cache);

static void
requirements_free (GogRequirements *r)
{
  g_free (r->os);
  g_free (r->level);
  g_strfreev (r->names);
  g_strfreev (r->values);
  g_free (r);
}

static GogStoreInfo *
store_info_new (void)
{
  GogStoreInfo *info = g_new0 (GogStoreInfo, 1);
  info->requirements = g_ptr_array_new_with_free_func ((GDestroyNotify) requirements_free);
  return info;
}

void
gog_store_info_free (GogStoreInfo *info)
{
  if (!info)
    return;
  g_strfreev (info->features);
  g_strfreev (info->tags);
  g_strfreev (info->audio_languages);
  g_strfreev (info->text_languages);
  g_strfreev (info->age_ratings);
  g_strfreev (info->editions);
  g_strfreev (info->technologies);
  g_ptr_array_unref (info->requirements);
  g_free (info->copyright);
  g_free (info);
}

static gboolean
strv_same (GStrv a, GStrv b)
{
  return (!a && !b) || (a && b && g_strv_equal ((const char *const *) a, (const char *const *) b));
}

gboolean
gog_store_info_equal (const GogStoreInfo *a, const GogStoreInfo *b)
{
  if (!a || !b)
    return a == b;
  if (!(strv_same (a->features, b->features) && strv_same (a->tags, b->tags) &&
        strv_same (a->audio_languages, b->audio_languages) &&
        strv_same (a->text_languages, b->text_languages) &&
        strv_same (a->age_ratings, b->age_ratings) && strv_same (a->editions, b->editions) &&
        strv_same (a->technologies, b->technologies) &&
        g_strcmp0 (a->copyright, b->copyright) == 0 && a->in_development == b->in_development &&
        a->requirements->len == b->requirements->len))
    return FALSE;
  for (guint i = 0; i < a->requirements->len; i++)
    {
      GogRequirements *ra = g_ptr_array_index (a->requirements, i);
      GogRequirements *rb = g_ptr_array_index (b->requirements, i);
      if (g_strcmp0 (ra->os, rb->os) != 0 || g_strcmp0 (ra->level, rb->level) != 0 ||
          !strv_same (ra->names, rb->names) || !strv_same (ra->values, rb->values))
        return FALSE;
    }
  return TRUE;
}

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

static json_object *
strv_to_json (GStrv strv)
{
  json_object *list = json_object_new_array ();
  for (GStrv s = strv; s && *s; s++)
    json_object_array_add (list, json_object_new_string (*s));
  return list;
}

static GStrv
json_to_strv (json_object *list)
{
  GStrvBuilder *b = g_strv_builder_new ();
  for (size_t i = 0; list && json_object_is_type (list, json_type_array) &&
                     i < json_object_array_length (list);
       i++)
    {
      json_object *item = json_object_array_get_idx (list, i);
      if (json_object_is_type (item, json_type_string))
        g_strv_builder_add (b, json_object_get_string (item));
    }
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return out;
}

static json_object *
store_info_to_json (const GogStoreInfo *info)
{
  json_object *o = json_object_new_object ();
  json_object_object_add (o, "features", strv_to_json (info->features));
  json_object_object_add (o, "tags", strv_to_json (info->tags));
  json_object_object_add (o, "audio_languages", strv_to_json (info->audio_languages));
  json_object_object_add (o, "text_languages", strv_to_json (info->text_languages));
  json_object_object_add (o, "age_ratings", strv_to_json (info->age_ratings));
  json_object_object_add (o, "editions", strv_to_json (info->editions));
  json_object_object_add (o, "technologies", strv_to_json (info->technologies));
  json_object *reqs = json_object_new_array ();
  for (guint i = 0; i < info->requirements->len; i++)
    {
      GogRequirements *r = g_ptr_array_index (info->requirements, i);
      json_object *ro = json_object_new_object ();
      json_object_object_add (ro, "os", json_object_new_string (r->os));
      json_object_object_add (ro, "level", json_object_new_string (r->level));
      json_object_object_add (ro, "names", strv_to_json (r->names));
      json_object_object_add (ro, "values", strv_to_json (r->values));
      json_object_array_add (reqs, ro);
    }
  json_object_object_add (o, "requirements", reqs);
  if (info->copyright)
    json_object_object_add (o, "copyright", json_object_new_string (info->copyright));
  json_object_object_add (o, "in_development", json_object_new_boolean (info->in_development));
  return o;
}

static GogStoreInfo *
store_info_from_json (json_object *o)
{
  GogStoreInfo *info = store_info_new ();
  info->features = json_to_strv (ju_get_array (o, "features"));
  info->tags = json_to_strv (ju_get_array (o, "tags"));
  info->audio_languages = json_to_strv (ju_get_array (o, "audio_languages"));
  info->text_languages = json_to_strv (ju_get_array (o, "text_languages"));
  info->age_ratings = json_to_strv (ju_get_array (o, "age_ratings"));
  info->editions = json_to_strv (ju_get_array (o, "editions"));
  info->technologies = json_to_strv (ju_get_array (o, "technologies"));
  json_object *reqs = ju_get_array (o, "requirements");
  for (size_t i = 0; reqs && i < json_object_array_length (reqs); i++)
    {
      json_object *ro = json_object_array_get_idx (reqs, i);
      const char *os = ju_get_string (ro, "os", NULL);
      const char *level = ju_get_string (ro, "level", NULL);
      GStrv names = json_to_strv (ju_get_array (ro, "names"));
      GStrv values = json_to_strv (ju_get_array (ro, "values"));
      /* The names belong to the values; a hand-edited file could pair them
       * wrong. */
      if (!os || !level || g_strv_length (names) != g_strv_length (values))
        {
          g_strfreev (names);
          g_strfreev (values);
          continue;
        }
      GogRequirements *r = g_new0 (GogRequirements, 1);
      r->os = g_strdup (os);
      r->level = g_strdup (level);
      r->names = names;
      r->values = values;
      g_ptr_array_add (info->requirements, r);
    }
  info->copyright = g_strdup (ju_get_string (o, "copyright", NULL));
  info->in_development = ju_get_bool (o, "in_development", FALSE);
  return info;
}

GHashTable *
gog_store_info_load (void)
{
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) gog_store_info_free);
  G_LOCK (cache);
  json_object *root = load_cache_json ();
  G_UNLOCK (cache);
  json_object_object_foreach (root, key, entry)
    {
      json_object *store = ju_get_obj (entry, "store");
      if (store)
        g_hash_table_insert (table, g_strdup (key), store_info_from_json (store));
    }
  json_object_put (root);
  return table;
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

void
gog_categories_forget (void)
{
  G_LOCK (cache);
  gchar *path = gog_config_app_file (CACHE_FILE);
  if (g_remove (path) != 0 && errno != ENOENT)
    g_warning ("Could not remove %s: %s", path, g_strerror (errno));
  g_free (path);
  G_UNLOCK (cache);
}

GStrv
gog_categories_parse (const char *json)
{
  json_object *root = json ? ju_parse_string (json) : NULL;
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

/* ---- reading the rest of the answer ---- */

/* The "name" of each object in a list, without repeats or empty ones. */
static void
add_names (GStrvBuilder *b, GPtrArray *seen, json_object *list, const char *key)
{
  for (size_t i = 0; list && i < json_object_array_length (list); i++)
    {
      const char *name = ju_get_string (json_object_array_get_idx (list, i), key, NULL);
      if (!name || !*name || g_ptr_array_find_with_equal_func (seen, name, g_str_equal, NULL))
        continue;
      g_ptr_array_add (seen, (gpointer) name);
      g_strv_builder_add (b, name);
    }
}

static GStrv
names (json_object *list, const char *key)
{
  GStrvBuilder *b = g_strv_builder_new ();
  GPtrArray *seen = g_ptr_array_new ();
  add_names (b, seen, list, key);
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_ptr_array_unref (seen);
  return out;
}

/* The languages of one scope ("audio" or "text"), in GOG's order. */
static GStrv
localizations (json_object *list, const char *scope)
{
  GStrvBuilder *b = g_strv_builder_new ();
  GPtrArray *seen = g_ptr_array_new ();
  for (size_t i = 0; list && i < json_object_array_length (list); i++)
    {
      json_object *e = ju_get_obj (json_object_array_get_idx (list, i), "_embedded");
      const char *name = ju_get_string (ju_get_obj (e, "language"), "name", NULL);
      const char *type = ju_get_string (ju_get_obj (e, "localizationScope"), "type", NULL);
      if (!name || !*name || g_strcmp0 (type, scope) != 0 ||
          g_ptr_array_find_with_equal_func (seen, name, g_str_equal, NULL))
        continue;
      g_ptr_array_add (seen, (gpointer) name);
      g_strv_builder_add (b, name);
    }
  GStrv out = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_ptr_array_unref (seen);
  return out;
}

/* "PEGI 18 (Bad Language, Violence)" from {"pegiRating": {"ageRating": 18,
 * "contentDescriptors": [...]}}; ESRB goes by its category ("Mature 17+"),
 * the others by age. */
static gchar *
age_rating (const char *key, json_object *rating)
{
  gsize len = strlen (key) - strlen ("Rating");
  gchar *board = g_ascii_strup (key, len);
  const char *category = ju_get_string (ju_get_obj (rating, "category"), "name", NULL);
  gint64 age = ju_get_int64 (rating, "ageRating", -1);
  GString *text = g_string_new (board);
  if (g_strcmp0 (board, "ESRB") == 0 && category)
    g_string_append_printf (text, " %s", category);
  else if (age >= 0)
    g_string_append_printf (text, " %" G_GINT64_FORMAT, age);
  else if (category)
    g_string_append_printf (text, " %s", category);
  else
    {
      g_string_free (text, TRUE);
      g_free (board);
      return NULL;
    }
  GStrv descriptors = names (ju_get_array (rating, "contentDescriptors"), "descriptor");
  /* GOG's sometimes end in a line break ("Violência\n"). */
  for (GStrv d = descriptors; *d; d++)
    g_strstrip (*d);
  if (descriptors[0])
    {
      gchar *joined = g_strjoinv (", ", descriptors);
      g_string_append_printf (text, " (%s)", joined);
      g_free (joined);
    }
  g_strfreev (descriptors);
  g_free (board);
  return g_string_free (text, FALSE);
}

static const char *
os_label (const char *name)
{
  if (g_strcmp0 (name, "windows") == 0)
    return "Windows";
  if (g_strcmp0 (name, "osx") == 0 || g_strcmp0 (name, "mac") == 0)
    return "macOS";
  if (g_strcmp0 (name, "linux") == 0)
    return "Linux";
  return name;
}

/* Text as GOG writes it (it may hold HTML), plain; NULL if empty. */
static gchar *
plain_text (const char *html)
{
  gchar *text = html ? text_strip_html (html) : NULL;
  if (text && !*text)
    g_clear_pointer (&text, g_free);
  return text;
}

static void
add_requirements (GogStoreInfo *info, json_object *systems)
{
  for (size_t i = 0; systems && i < json_object_array_length (systems); i++)
    {
      json_object *system = json_object_array_get_idx (systems, i);
      const char *os = ju_get_string (ju_get_obj (system, "operatingSystem"), "name", NULL);
      json_object *sets = ju_get_array (system, "systemRequirements");
      for (size_t j = 0; os && sets && j < json_object_array_length (sets); j++)
        {
          json_object *set = json_object_array_get_idx (sets, j);
          const char *type = ju_get_string (set, "type", NULL);
          GStrvBuilder *keys = g_strv_builder_new ();
          GStrvBuilder *values = g_strv_builder_new ();
          guint n = 0;
          json_object *list = ju_get_array (set, "requirements");
          for (size_t k = 0; list && k < json_object_array_length (list); k++)
            {
              json_object *item = json_object_array_get_idx (list, k);
              gchar *value = plain_text (ju_get_string (item, "description", NULL));
              const char *name = ju_get_string (item, "name", NULL);
              if (!value || !name || !*name)
                {
                  g_free (value);
                  continue;
                }
              /* "Processor:" */
              gchar *key = g_strstrip (g_strdup (name));
              gsize len = strlen (key);
              if (len > 1 && key[len - 1] == ':')
                key[len - 1] = '\0';
              g_strv_builder_take (keys, key);
              g_strv_builder_take (values, value);
              n++;
            }
          /* Older pages have only a block of text. */
          gchar *text = n == 0 ? plain_text (ju_get_string (set, "description", NULL)) : NULL;
          if (text)
            {
              g_strv_builder_add (keys, "Requirements");
              g_strv_builder_take (values, text);
              n++;
            }
          GStrv key_list = g_strv_builder_end (keys);
          GStrv value_list = g_strv_builder_end (values);
          g_strv_builder_unref (keys);
          g_strv_builder_unref (values);
          if (n == 0)
            {
              g_strfreev (key_list);
              g_strfreev (value_list);
              continue;
            }
          GogRequirements *r = g_new0 (GogRequirements, 1);
          r->os = g_strdup (os_label (os));
          r->level = g_strcmp0 (type, "recommended") == 0 ? g_strdup ("Recommended")
                                                           : g_strdup ("Minimum");
          r->names = key_list;
          r->values = value_list;
          g_ptr_array_add (info->requirements, r);
        }
    }
}

GogStoreInfo *
gog_store_info_parse (const char *json)
{
  json_object *root = json ? ju_parse_string (json) : NULL;
  json_object *e = ju_get_obj (root, "_embedded");
  if (!root || !json_object_is_type (root, json_type_object) || !e)
    {
      if (root)
        json_object_put (root);
      return NULL;
    }
  GogStoreInfo *info = store_info_new ();
  info->features = names (ju_get_array (e, "features"), "name");
  info->tags = names (ju_get_array (e, "properties"), "name");
  json_object *locs = ju_get_array (e, "localizations");
  info->audio_languages = localizations (locs, "audio");
  info->text_languages = localizations (locs, "text");

  GStrvBuilder *b = g_strv_builder_new ();
  json_object_object_foreach (e, key, value)
    {
      if (!g_str_has_suffix (key, "Rating") || strlen (key) <= strlen ("Rating") ||
          !json_object_is_type (value, json_type_object))
        continue;
      gchar *text = age_rating (key, value);
      if (text)
        g_strv_builder_take (b, text);
    }
  info->age_ratings = g_strv_builder_end (b);
  g_strv_builder_unref (b);

  json_object *editions = ju_get_array (e, "editions");
  info->editions = editions && json_object_array_length (editions) > 1
                       ? names (editions, "name")
                       : g_new0 (gchar *, 1);

  /* Strings, or objects with a name. */
  b = g_strv_builder_new ();
  GPtrArray *seen = g_ptr_array_new ();
  json_object *tech = ju_get_array (root, "technologies");
  for (size_t i = 0; tech && i < json_object_array_length (tech); i++)
    {
      json_object *item = json_object_array_get_idx (tech, i);
      const char *name = json_object_is_type (item, json_type_string)
                             ? json_object_get_string (item)
                             : ju_get_string (item, "name", NULL);
      if (!name || !*name || g_ptr_array_find_with_equal_func (seen, name, g_str_equal, NULL))
        continue;
      g_ptr_array_add (seen, (gpointer) name);
      g_strv_builder_add (b, name);
    }
  if (ju_get_bool (root, "isUsingDosBox", FALSE) &&
      !g_ptr_array_find_with_equal_func (seen, "DOSBox", g_str_equal, NULL))
    g_strv_builder_add (b, "DOSBox");
  info->technologies = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  g_ptr_array_unref (seen);

  add_requirements (info, ju_get_array (e, "supportedOperatingSystems"));
  info->copyright = plain_text (ju_get_string (root, "copyrights", NULL));
  info->in_development = ju_get_bool (ju_get_obj (root, "inDevelopment"), "active", FALSE);
  json_object_put (root);
  return info;
}

/* store NULL: GOG doesn't know the game. */
static void
record (json_object *cache, gint64 id, GStrv categories, const GogStoreInfo *store, gint64 now)
{
  json_object *entry = json_object_new_object ();
  json_object_object_add (entry, "categories", strv_to_json (categories));
  if (store)
    json_object_object_add (entry, "store", store_info_to_json (store));
  json_object_object_add (entry, "checked", json_object_new_int64 (now));
  json_object_object_add (entry, "version", json_object_new_int (CACHE_VERSION));
  gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, id);
  json_object_object_add (cache, key, entry);
  g_free (key);
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
          GStrv categories = g_file_get_contents (dests[i], &body, NULL, NULL)
                                 ? gog_categories_parse (body)
                                 : NULL;
          GogStoreInfo *store = categories ? gog_store_info_parse (body) : NULL;
          g_free (body);
          if (categories)
            {
              record (cache, ids[i], categories, store, now);
              fetched++;
            }
          g_strfreev (categories);
          gog_store_info_free (store);
        }
      else if (codes[i] == 404)
        {
          /* No longer sold, or never a product of its own: GOG has none. */
          record (cache, ids[i], NULL, NULL, now);
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not fetch categories from GOG: %s",
                   curl_error ? curl_error : "no answer");
      ok = FALSE;
    }
  g_free (curl_error);
  return ok;
}

gboolean
gog_categories_lookup (const gint64 *ids, guint n_ids, GCancellable *cancellable,
                       GogCategoriesProgress progress, gpointer user_data, guint *looked_up,
                       GError **error)
{
  /* The lock is held only while the file is read or written, never across
   * the network: the UI thread takes it in gog_categories_load(). */
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
      json_object *list = ju_get_array (entry, "categories");
      gboolean known = list && json_object_array_length (list) > 0;
      gboolean current = ju_get_int64 (entry, "version", 1) >= CACHE_VERSION;
      if (entry && ((known && current) ||
                    (!known && now - ju_get_int64 (entry, "checked", 0) < RETRY_SECONDS)))
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
        progress (i + n, todo->len, user_data);
    }
  g_array_unref (todo);
  json_object_put (cache);
  return ok;
}
