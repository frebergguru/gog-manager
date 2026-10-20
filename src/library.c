/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "library.h"
#include "json_util.h"
#include "text_util.h"
#include <glib/gstdio.h>
#include <string.h>

struct _GogLibrary
{
  GObject parent_instance;
  GListStore *store; /* GogGame */
  char *root_path;
};

enum
{
  SIG_SCAN_PROGRESS,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GogLibrary, gog_library, G_TYPE_OBJECT)

static void
gog_library_finalize (GObject *object)
{
  GogLibrary *self = GOG_LIBRARY (object);
  g_clear_object (&self->store);
  g_free (self->root_path);
  G_OBJECT_CLASS (gog_library_parent_class)->finalize (object);
}

static void
gog_library_class_init (GogLibraryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_library_finalize;

  signals[SIG_SCAN_PROGRESS] =
      g_signal_new ("scan-progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_DOUBLE, G_TYPE_STRING);
}

static void
gog_library_init (GogLibrary *self)
{
  self->store = g_list_store_new (GOG_TYPE_GAME);
}

GogLibrary *
gog_library_new (void)
{
  return g_object_new (GOG_TYPE_LIBRARY, NULL);
}

GListModel *
gog_library_get_model (GogLibrary *self)
{
  return G_LIST_MODEL (self->store);
}

GogGame *
gog_library_find_by_id (GogLibrary *self, gint64 id)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      g_object_unref (g); /* the store keeps it alive; transfer none */
      if (gog_game_get_id (g) == id)
        return g;
    }
  return NULL;
}

/* g_ptr_array_sort passes pointers to the elements, not the strings.
 * Case-folded, so "bahasa Indonesia" sorts among the Bs, not after "Türkçe". */
static gint
compare_language_names (gconstpointer a, gconstpointer b)
{
  gchar *fa = g_utf8_casefold (*(const gchar *const *) a, -1);
  gchar *fb = g_utf8_casefold (*(const gchar *const *) b, -1);
  gint result = g_utf8_collate (fa, fb);
  g_free (fa);
  g_free (fb);
  return result;
}

GStrv
gog_library_get_all_languages (GogLibrary *self)
{
  /* Set of names (keys owned by the table) so each language is listed once
   * however many games mention it. */
  GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      for (const char *const *l = gog_game_get_languages (g); l && *l; l++)
        g_hash_table_add (seen, g_strdup (*l));
      GStrv downloaded = gog_game_dup_installer_languages (g);
      for (GStrv l = downloaded; *l; l++)
        g_hash_table_add (seen, g_strdup (*l));
      g_strfreev (downloaded);
      g_object_unref (g);
    }

  GPtrArray *out = g_ptr_array_new ();
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init (&iter, seen);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    g_ptr_array_add (out, g_strdup (key));
  g_hash_table_unref (seen);
  g_ptr_array_sort (out, compare_language_names);
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

/* ---- disk scanning / parsing helpers ---- */

static char *
find_prefixed_file (const char *dir, const char *prefix)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  char *found = NULL;
  while ((name = g_dir_read_name (d)))
    {
      if (g_str_has_prefix (name, prefix))
        {
          found = g_build_filename (dir, name, NULL);
          break;
        }
    }
  g_dir_close (d);
  return found;
}

static GogOs
os_from_extension (const char *filename)
{
  gchar *lower = g_ascii_strdown (filename, -1);
  GogOs os = GOG_OS_WINDOWS;
  if (g_str_has_suffix (lower, ".exe"))
    os = GOG_OS_WINDOWS;
  else if (g_str_has_suffix (lower, ".sh"))
    os = GOG_OS_LINUX;
  else if (g_str_has_suffix (lower, ".pkg") || g_str_has_suffix (lower, ".dmg"))
    os = GOG_OS_MAC;
  g_free (lower);
  return os;
}

static gboolean
is_installer_file (const char *filename)
{
  gchar *lower = g_ascii_strdown (filename, -1);
  gboolean ok = (g_str_has_suffix (lower, ".exe") ||
                 g_str_has_suffix (lower, ".sh") ||
                 g_str_has_suffix (lower, ".pkg") ||
                 g_str_has_suffix (lower, ".dmg")) &&
                !g_regex_match_simple ("-\\d+\\.bin$", filename, G_REGEX_CASELESS, 0);
  g_free (lower);
  return ok;
}

/* GOG names extra-language installers with an English tag, e.g.
 * "setup_foo_1.1_(german)_(24519).exe" or "foo_(french_64bit)_(123).exe";
 * the untagged installer is the English one. Codes and names are the ones
 * GOG uses in its product metadata, so they merge with its language lists. */
typedef struct
{
  const char *tag;
  const char *code;
  const char *name;
} FileLanguage;

static const FileLanguage file_languages[] = {
  { "english", "en", "English" },
  { "german", "de", "Deutsch" },
  { "french", "fr", "français" },
  { "spanish", "es", "español" },
  { "spanish_latam", "es_mx", "Español (AL)" },
  { "italian", "it", "italiano" },
  { "polish", "pl", "polski" },
  { "russian", "ru", "русский" },
  { "czech", "cz", "český" },
  { "cs", "cz", "český" },
  { "hungarian", "hu", "magyar" },
  { "dutch", "nl", "nederlands" },
  { "portuguese", "pt", "português" },
  { "portuguese_brazilian", "br", "Português do Brasil" },
  { "brazilian", "br", "Português do Brasil" },
  { "swedish", "sv", "svenska" },
  { "danish", "da", "Dansk" },
  { "finnish", "fi", "suomi" },
  { "norwegian", "no", "norsk" },
  { "turkish", "tr", "Türkçe" },
  { "ukrainian", "uk", "yкраїнська" },
  { "greek", "gk", "Ελληνικά" },
  { "romanian", "ro", "română" },
  { "bulgarian", "bl", "български" },
  { "slovak", "sk", "slovenský" },
  { "arabic", "ar", "العربية" },
  { "thai", "th", "ไทย" },
  { "korean", "ko", "한국어" },
  { "japanese", "jp", "日本語" },
  { "chinese", "cn", "中文" },
  { "chinese_simplified", "cn", "中文" },
  { "chinese_traditional", "zh", "中文(繁體)" },
};

gboolean
gog_library_language_from_installer_name (const char *filename, const char **out_code,
                                          const char **out_name)
{
  for (const char *open = strchr (filename, '('); open; open = strchr (open + 1, '('))
    {
      const char *close = strchr (open, ')');
      if (!close)
        break;
      gchar *tag = g_ascii_strdown (open + 1, close - open - 1);
      if (g_str_has_suffix (tag, "_64bit") || g_str_has_suffix (tag, "_32bit"))
        tag[strlen (tag) - strlen ("_64bit")] = '\0';
      for (gsize i = 0; i < G_N_ELEMENTS (file_languages); i++)
        if (g_strcmp0 (tag, file_languages[i].tag) == 0)
          {
            g_free (tag);
            if (out_code)
              *out_code = file_languages[i].code;
            if (out_name)
              *out_name = file_languages[i].name;
            return TRUE;
          }
      g_free (tag);
    }
  return FALSE;
}

/* Sum the size of a main installer file plus any split "<stem>-N.bin"
 * sibling parts (GOG's split InnoSetup archives). */
static guint64
installer_total_size (const char *dir, const char *filename)
{
  gchar *full = g_build_filename (dir, filename, NULL);
  GStatBuf st;
  guint64 total = 0;
  if (g_stat (full, &st) == 0)
    total = (guint64) st.st_size;
  g_free (full);

  const char *dot = strrchr (filename, '.');
  if (!dot)
    return total;
  gchar *stem = g_strndup (filename, (gsize) (dot - filename));

  for (int part = 1; part < 50; part++)
    {
      gchar *partname = g_strdup_printf ("%s-%d.bin", stem, part);
      gchar *partpath = g_build_filename (dir, partname, NULL);
      if (g_stat (partpath, &st) == 0)
        total += (guint64) st.st_size;
      else
        {
          g_free (partname);
          g_free (partpath);
          break;
        }
      g_free (partname);
      g_free (partpath);
    }
  g_free (stem);
  return total;
}

/* Reads <dir>/serials.txt (name matched case-insensitively), the CD keys
 * GOG lists for some products. Returns trimmed, valid UTF-8, or NULL if
 * there is none. */
static gchar *
read_serials (const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  gchar *path = NULL;
  while (!path && (name = g_dir_read_name (d)))
    if (g_ascii_strcasecmp (name, "serials.txt") == 0)
      path = g_build_filename (dir, name, NULL);
  g_dir_close (d);
  if (!path)
    return NULL;

  gchar *contents = NULL;
  gsize len = 0;
  gboolean ok = g_file_get_contents (path, &contents, &len, NULL);
  g_free (path);
  if (!ok)
    return NULL;
  if (len > 64 * 1024) /* a key list, not a document; don't load junk */
    {
      g_free (contents);
      return NULL;
    }

  gchar *valid = g_utf8_make_valid (contents, (gssize) len);
  g_free (contents);
  /* Windows line endings would show up as stray characters. */
  gchar **lines = g_strsplit (valid, "\r\n", -1);
  gchar *joined = g_strjoinv ("\n", lines);
  g_strfreev (lines);
  g_free (valid);
  g_strstrip (joined);
  if (!*joined)
    {
      g_free (joined);
      return NULL;
    }
  return joined;
}

/* Find the downloads.installers[] entry that best matches an on-disk
 * installer file: same OS family, closest total_size. Returns NULL if
 * installers_arr is empty/absent. */
/* The metadata entry for an installer file: same OS, in language
 * want_code when such an entry exists (sizes alone can't tell languages
 * apart; several often share one size), closest in size otherwise. */
static json_object *
match_json_installer (json_object *installers_arr, GogOs os, guint64 disk_size,
                      const char *want_code)
{
  if (!installers_arr)
    return NULL;
  json_object *best = NULL;
  guint64 best_diff = G_MAXUINT64;
  gboolean best_has_lang = FALSE;
  size_t n = json_object_array_length (installers_arr);
  for (size_t i = 0; i < n; i++)
    {
      json_object *entry = json_object_array_get_idx (installers_arr, i);
      const char *entry_os = ju_get_string (entry, "os", "");
      GogOs eos = g_strcmp0 (entry_os, "windows") == 0 ? GOG_OS_WINDOWS :
                  g_strcmp0 (entry_os, "linux") == 0   ? GOG_OS_LINUX :
                                                          GOG_OS_MAC;
      if (eos != os)
        continue;
      gboolean has_lang = g_strcmp0 (ju_get_string (entry, "language", ""), want_code) == 0;
      if (best_has_lang && !has_lang)
        continue;
      if (has_lang && !best_has_lang)
        {
          best_has_lang = TRUE;
          best_diff = G_MAXUINT64;
        }
      guint64 total = (guint64) ju_get_int64 (entry, "total_size", 0);
      guint64 diff = total > disk_size ? total - disk_size : disk_size - total;
      if (diff < best_diff)
        {
          best_diff = diff;
          best = entry;
        }
    }
  return best;
}

static json_object *
match_json_bonus (json_object *bonus_arr, guint64 disk_size)
{
  if (!bonus_arr)
    return NULL;
  json_object *best = NULL;
  guint64 best_diff = G_MAXUINT64;
  size_t n = json_object_array_length (bonus_arr);
  for (size_t i = 0; i < n; i++)
    {
      json_object *entry = json_object_array_get_idx (bonus_arr, i);
      guint64 total = (guint64) ju_get_int64 (entry, "total_size", 0);
      guint64 diff = total > disk_size ? total - disk_size : disk_size - total;
      if (diff < best_diff)
        {
          best_diff = diff;
          best = entry;
        }
    }
  return best;
}

static void
scan_installers (GogGame *game, const char *dir, json_object *root)
{
  json_object *downloads = ju_get_obj (root, "downloads");
  json_object *installers_arr = ju_get_array (downloads, "installers");

  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d)
    return;
  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      if (!is_installer_file (name))
        continue;

      GogOs os = os_from_extension (name);
      guint64 size = installer_total_size (dir, name);
      const char *tag_code = NULL, *tag_name = NULL;
      gboolean tagged = gog_library_language_from_installer_name (name, &tag_code, &tag_name);
      json_object *meta = match_json_installer (installers_arr, os, size,
                                                tagged ? tag_code : "en");

      GogInstallerInfo *info = gog_installer_info_new ();
      info->os = os;
      info->file_path = g_build_filename (dir, name, NULL);
      info->size = size;
      if (tagged && g_strcmp0 (ju_get_string (meta, "language", ""), tag_code) != 0)
        {
          /* The file name is more reliable than a size guess. */
          info->language_code = g_strdup (tag_code);
          info->language_full = g_strdup (tag_name);
        }
      else
        {
          info->language_code = g_strdup (ju_get_string (meta, "language", "en"));
          info->language_full = g_strdup (ju_get_string (meta, "language_full", "English"));
        }
      info->version = g_strdup (ju_get_string (meta, "version", ""));
      gog_game_add_installer (game, info);
    }
  g_dir_close (d);
}

static void
scan_extras (GogGame *game, const char *dir, json_object *root)
{
  gchar *extras_dir = g_build_filename (dir, "extras", NULL);
  if (!g_file_test (extras_dir, G_FILE_TEST_IS_DIR))
    {
      g_free (extras_dir);
      return;
    }

  json_object *downloads = ju_get_obj (root, "downloads");
  json_object *bonus_arr = ju_get_array (downloads, "bonus_content");

  GDir *d = g_dir_open (extras_dir, 0, NULL);
  if (!d)
    {
      g_free (extras_dir);
      return;
    }
  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      gchar *full = g_build_filename (extras_dir, name, NULL);
      GStatBuf st;
      guint64 size = (g_stat (full, &st) == 0) ? (guint64) st.st_size : 0;

      json_object *meta = match_json_bonus (bonus_arr, size);

      GogExtraInfo *info = gog_extra_info_new ();
      info->file_path = full; /* transfer */
      info->size = size;
      info->name = g_strdup (ju_get_string (meta, "name", name));
      info->type = g_strdup (ju_get_string (meta, "type", "file"));
      gog_game_add_extra (game, info);
    }
  g_dir_close (d);
  g_free (extras_dir);
}


/* A page on GOG's own site; NULL for anything else. */
static gchar *
gog_page (json_object *links, const char *key)
{
  const char *url = ju_get_string (links, key, NULL);
  return url && g_str_has_prefix (url, "https://www.gog.com/") ? g_strdup (url) : NULL;
}

static gboolean
plain_id (const char *id, gboolean hex_only)
{
  if (!id || !*id)
    return FALSE;
  for (const char *c = id; *c; c++)
    if (hex_only ? !g_ascii_isxdigit (*c) : !(g_ascii_isalnum (*c) || *c == '-' || *c == '_'))
      return FALSE;
  return TRUE;
}

/* The product json's details beyond what the scan uses itself. */
static GogProductInfo *
product_info (json_object *root)
{
  GogProductInfo *info = gog_product_info_new ();
  gchar *lead = text_strip_html (ju_get_string (ju_get_obj (root, "description"), "lead", ""));
  info->lead = *lead ? lead : (g_free (lead), NULL);
  gchar *changelog = text_strip_html (ju_get_string (root, "changelog", ""));
  info->changelog = *changelog ? changelog : (g_free (changelog), NULL);

  json_object *links = ju_get_obj (root, "links");
  info->store_url = gog_page (links, "product_card");
  info->support_url = gog_page (links, "support");
  info->forum_url = gog_page (links, "forum");
  info->in_development = ju_get_bool (ju_get_obj (root, "in_development"), "active", FALSE);

  /* {"image_id": "<hash>", ...}: GOG's image server makes any size of it,
   * as for the games database's. */
  GStrvBuilder *b = g_strv_builder_new ();
  json_object *shots = ju_get_array (root, "screenshots");
  for (size_t i = 0; shots && i < json_object_array_length (shots); i++)
    {
      const char *id = ju_get_string (json_object_array_get_idx (shots, i), "image_id", NULL);
      if (plain_id (id, TRUE))
        g_strv_builder_take (b, g_strdup_printf ("https://images.gog-statics.com/%s{formatter}.{ext}",
                                                 id));
    }
  g_strfreev (info->screenshots);
  info->screenshots = g_strv_builder_end (b);
  g_strv_builder_unref (b);

  /* "https://www.youtube.com/embed/<id>?wmode=opaque&rel=0" */
  b = g_strv_builder_new ();
  json_object *videos = ju_get_array (root, "videos");
  for (size_t i = 0; videos && i < json_object_array_length (videos); i++)
    {
      json_object *video = json_object_array_get_idx (videos, i);
      const char *url = ju_get_string (video, "video_url", NULL);
      const char *embed = url ? strstr (url, "youtube.com/embed/") : NULL;
      if (g_strcmp0 (ju_get_string (video, "provider", NULL), "youtube") != 0 || !embed)
        continue;
      embed += strlen ("youtube.com/embed/");
      gchar *id = g_strndup (embed, strcspn (embed, "?&#/"));
      if (plain_id (id, FALSE))
        g_strv_builder_take (b, id);
      else
        g_free (id);
    }
  g_strfreev (info->video_ids);
  info->video_ids = g_strv_builder_end (b);
  g_strv_builder_unref (b);
  return info;
}

static GogGame *
parse_game_dir (const char *dir_path)
{
  gchar *product_json_path = find_prefixed_file (dir_path, "product_");
  if (!product_json_path)
    return NULL;

  GError *error = NULL;
  json_object *root = ju_load_file (product_json_path, &error);
  g_free (product_json_path);
  if (!root)
    {
      g_warning ("Failed to parse product json in %s: %s", dir_path,
                 error ? error->message : "unknown error");
      g_clear_error (&error);
      return NULL;
    }

  gchar *basename = g_path_get_basename (dir_path);

  GogGame *game = gog_game_new ();
  gog_game_set_id (game, ju_get_int64 (root, "id", 0));
  /* The slug names the install folder; one that would leave it is not used. */
  const char *slug = ju_get_string (root, "slug", basename);
  if (!slug || !*slug || strchr (slug, '/') || strcmp (slug, ".") == 0 || strcmp (slug, "..") == 0)
    slug = basename;
  gog_game_set_slug (game, slug);
  gog_game_set_title (game, ju_get_string (root, "title", basename));
  gog_game_set_folder_path (game, dir_path);

  /* e.g. "2021-09-29T17:50:00+0200"; missing or null for some games. */
  const char *released = ju_get_string (root, "release_date", NULL);
  GDateTime *released_dt = released ? g_date_time_new_from_iso8601 (released, NULL) : NULL;
  if (released_dt)
    {
      gog_game_set_release_date (game, g_date_time_to_unix (released_dt));
      g_date_time_unref (released_dt);
    }

  const char *game_type = ju_get_string (root, "game_type", "game");
  gog_game_set_product_type (game, g_strcmp0 (game_type, "dlc") == 0 ? GOG_PRODUCT_DLC
                                                                      : GOG_PRODUCT_GAME);

  json_object *desc = ju_get_obj (root, "description");
  gchar *plain = text_strip_html (ju_get_string (desc, "full", ""));
  gog_game_set_description (game, plain);
  g_free (plain);

  json_object *compat = ju_get_obj (root, "content_system_compatibility");
  gog_game_set_supports_os (game, GOG_OS_WINDOWS, ju_get_bool (compat, "windows", FALSE));
  gog_game_set_supports_os (game, GOG_OS_LINUX, ju_get_bool (compat, "linux", FALSE));
  gog_game_set_supports_os (game, GOG_OS_MAC, ju_get_bool (compat, "osx", FALSE));

  gog_game_set_languages (game, ju_get_languages (root));
  gog_game_set_product_info (game, product_info (root));

  json_object *related = ju_get_array (root, "related_products");
  if (related)
    {
      size_t n = json_object_array_length (related);
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint64));
      for (size_t i = 0; i < n; i++)
        {
          json_object *item = json_object_array_get_idx (related, i);
          gint64 id = 0;
          if (json_object_is_type (item, json_type_int))
            id = json_object_get_int64 (item);
          else if (json_object_is_type (item, json_type_object))
            id = ju_get_int64 (item, "id", 0);
          if (id != 0)
            g_array_append_val (ids, id);
        }
      gog_game_set_related_ids (game, (const gint64 *) ids->data, ids->len);
      g_array_unref (ids);
    }

  gchar *icon = find_prefixed_file (dir_path, "icon_");
  if (icon)
    gog_game_set_icon_path (game, icon);
  g_free (icon);
  gchar *logo = find_prefixed_file (dir_path, "logo_");
  if (logo)
    gog_game_set_logo_path (game, logo);
  g_free (logo);

  gchar *serials = read_serials (dir_path);
  gog_game_set_serials (game, serials);
  g_free (serials);

  scan_installers (game, dir_path, root);
  scan_extras (game, dir_path, root);

  json_object_put (root);
  g_free (basename);
  return game;
}

/* ---- async scan ---- */

typedef struct
{
  char *root_path;
  GPtrArray *games; /* GogGame*, built on worker thread */
} ScanData;

static void
scan_data_free (ScanData *data)
{
  g_free (data->root_path);
  if (data->games)
    g_ptr_array_unref (data->games);
  g_free (data);
}

typedef struct
{
  GogLibrary *self; /* reffed: the idle can outlive the scan task */
  double fraction;
  gchar *msg;
} ProgressData;

static gboolean
emit_progress_idle (gpointer user_data)
{
  ProgressData *pd = user_data;
  g_signal_emit (pd->self, signals[SIG_SCAN_PROGRESS], 0, pd->fraction, pd->msg);
  g_object_unref (pd->self);
  g_free (pd->msg);
  g_free (pd);
  return G_SOURCE_REMOVE;
}

static void
report_progress (GogLibrary *self, double fraction, const char *msg)
{
  ProgressData *pd = g_new (ProgressData, 1);
  pd->self = g_object_ref (self);
  pd->fraction = fraction;
  pd->msg = g_strdup (msg);
  g_idle_add (emit_progress_idle, pd);
}

static void
scan_thread_func (GTask *task, gpointer source_object, gpointer task_data,
                   GCancellable *cancellable)
{
  GogLibrary *self = GOG_LIBRARY (source_object);
  ScanData *data = task_data;

  GDir *d = g_dir_open (data->root_path, 0, NULL);
  if (!d)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "Cannot open library directory: %s", data->root_path);
      return;
    }

  GPtrArray *entries = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  while ((name = g_dir_read_name (d)))
    g_ptr_array_add (entries, g_build_filename (data->root_path, name, NULL));
  g_dir_close (d);

  data->games = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < entries->len; i++)
    {
      if (g_cancellable_is_cancelled (cancellable))
        break;

      const char *path = g_ptr_array_index (entries, i);
      if (!g_file_test (path, G_FILE_TEST_IS_DIR))
        continue;

      GogGame *game = parse_game_dir (path);
      if (game)
        g_ptr_array_add (data->games, game);

      /* lgogdownloader keeps DLC inside its game: "<game>/dlc/<dlc>/". */
      gchar *dlc_root = g_build_filename (path, "dlc", NULL);
      GDir *dd = g_dir_open (dlc_root, 0, NULL);
      if (dd)
        {
          const char *dlc_name;
          while ((dlc_name = g_dir_read_name (dd)))
            {
              gchar *dlc_path = g_build_filename (dlc_root, dlc_name, NULL);
              GogGame *dlc = g_file_test (dlc_path, G_FILE_TEST_IS_DIR) ? parse_game_dir (dlc_path)
                                                                         : NULL;
              if (dlc)
                {
                  /* Its product file doesn't name the game; the folder does. */
                  if (game)
                    gog_game_set_base_game (dlc, game);
                  g_ptr_array_add (data->games, dlc);
                }
              g_free (dlc_path);
            }
          g_dir_close (dd);
        }
      g_free (dlc_root);

      double fraction = (double) (i + 1) / (double) MAX (entries->len, 1);
      gchar *msg = g_strdup_printf ("Scanning… %u / %u", i + 1, entries->len);
      report_progress (self, fraction, msg);
      g_free (msg);
    }

  g_ptr_array_unref (entries);
  g_task_return_boolean (task, TRUE);
}

void
gog_library_scan_async (GogLibrary *self, const char *root_path, GCancellable *cancellable,
                         GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (self, cancellable, callback, user_data);
  ScanData *data = g_new0 (ScanData, 1);
  data->root_path = g_strdup (root_path);
  g_task_set_task_data (task, data, (GDestroyNotify) scan_data_free);
  g_task_run_in_thread (task, scan_thread_func);
  g_object_unref (task);
}

gboolean
gog_library_scan_finish (GogLibrary *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);
  GTask *task = G_TASK (result);
  ScanData *data = g_task_get_task_data (task);
  gboolean ok = g_task_propagate_boolean (task, error);
  if (!ok)
    return FALSE;

  g_list_store_remove_all (self->store);
  for (guint i = 0; i < data->games->len; i++)
    g_list_store_append (self->store, g_ptr_array_index (data->games, i));

  /* Resolve DLC -> base game links now that everything is loaded. */
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->store));
  for (guint i = 0; i < n; i++)
    {
      GogGame *g = g_list_model_get_item (G_LIST_MODEL (self->store), i);
      if (gog_game_get_product_type (g) != GOG_PRODUCT_DLC)
        {
          g_object_unref (g);
          continue;
        }
      guint nrel = 0;
      const gint64 *rel = gog_game_get_related_ids (g, &nrel);
      for (guint r = 0; r < nrel; r++)
        {
          GogGame *base = gog_library_find_by_id (self, rel[r]);
          if (base && gog_game_get_product_type (base) == GOG_PRODUCT_GAME)
            {
              gog_game_set_base_game (g, base);
              break;
            }
        }
      g_object_unref (g);
    }

  return TRUE;
}
