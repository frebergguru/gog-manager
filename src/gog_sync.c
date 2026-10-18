/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gog_sync.h"
#include "original_language.h"
#include "config.h"
#include "json_util.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glib/gstdio.h>

#define LGOG "lgogdownloader"
#define RESULTS_FILE "gog-manager-updates.json"
#define SETTINGS_FILE "gog-manager-download.json"
#define MAX_SLOTS 16 /* download threads tracked; the setting allows 8 */

/* ---- items ---- */

static void
file_free (GogSyncFile *file)
{
  if (!file)
    return;
  g_free (file->gamename);
  g_free (file->filename);
  g_free (file->label);
  g_free (file->dlc_title);
  g_free (file);
}

static GogSyncItem *
item_new (const char *gamename)
{
  GogSyncItem *item = g_new0 (GogSyncItem, 1);
  item->gamename = g_strdup (gamename);
  item->files = g_ptr_array_new_with_free_func ((GDestroyNotify) file_free);
  return item;
}

void
gog_sync_item_free (GogSyncItem *item)
{
  if (!item)
    return;
  g_free (item->gamename);
  g_ptr_array_unref (item->files);
  g_free (item);
}

gboolean
gog_sync_is_available (void)
{
  gchar *path = g_find_program_in_path (LGOG);
  gboolean found = path != NULL;
  g_free (path);
  return found;
}

gchar *
gog_sync_pretty_name (const char *gamename)
{
  gchar *out = g_strdup (gamename);
  gboolean word_start = TRUE;
  for (gchar *p = out; *p; p++)
    {
      if (*p == '_')
        {
          *p = ' ';
          word_start = TRUE;
        }
      else
        {
          if (word_start)
            *p = g_ascii_toupper (*p);
          word_start = FALSE;
        }
    }
  return out;
}

/* ---- languages ---- */

/* Borrowed from lgogdownloader (include/globalconstants.h, WTFPL): its
 * languages in their bit order, as --list j reports them. */
static const GogSyncLanguage languages[] = {
  { "en", "English" },    { "de", "German" },     { "fr", "French" },
  { "pl", "Polish" },     { "ru", "Russian" },    { "cn", "Chinese" },
  { "cz", "Czech" },      { "es", "Spanish" },    { "hu", "Hungarian" },
  { "it", "Italian" },    { "jp", "Japanese" },   { "tr", "Turkish" },
  { "pt", "Portuguese" }, { "ko", "Korean" },     { "nl", "Dutch" },
  { "sv", "Swedish" },    { "no", "Norwegian" },  { "da", "Danish" },
  { "fi", "Finnish" },    { "br", "Brazilian Portuguese" },
  { "sk", "Slovak" },     { "bl", "Bulgarian" },  { "uk", "Ukrainian" },
  { "es_mx", "Spanish (Latin American)" },        { "ar", "Arabic" },
  { "ro", "Romanian" },   { "he", "Hebrew" },     { "th", "Thai" },
};

const GogSyncLanguage *
gog_sync_get_languages (guint *n_languages)
{
  *n_languages = G_N_ELEMENTS (languages);
  return languages;
}

static gchar *
languages_join (guint mask, gboolean names, const char *sep)
{
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (languages); i++)
    if (mask & (1u << i))
      g_string_append_printf (out, "%s%s", out->len ? sep : "",
                              names ? languages[i].name : languages[i].code);
  return g_string_free (out, FALSE);
}

gchar *
gog_sync_describe_languages (guint mask)
{
  return mask ? languages_join (mask, TRUE, ", ") : g_strdup ("All languages");
}

/* The --language value: "en+no", or "all". */
static gchar *
languages_arg (guint mask)
{
  return mask ? languages_join (mask, FALSE, "+") : g_strdup ("all");
}

static guint
languages_from_arg (const char *arg)
{
  guint mask = 0;
  gchar **codes = g_strsplit (arg ? arg : "", "+", -1);
  for (gchar **c = codes; *c; c++)
    for (guint i = 0; i < G_N_ELEMENTS (languages); i++)
      if (g_strcmp0 (*c, languages[i].code) == 0)
        mask |= 1u << i;
  g_strfreev (codes);
  return mask;
}

gboolean
gog_sync_file_wanted (const GogSyncFile *file, guint mask)
{
  return mask == 0 || file->part == GOG_SYNC_PART_EXTRA || file->languages == 0 ||
         (file->languages & mask) != 0;
}

static void save_results_checked (GPtrArray *items, gint64 checked_unix);

/* ---- remembered download choices ---- */

static json_object *
load_settings (void)
{
  gchar *path = gog_config_app_file (SETTINGS_FILE);
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
save_settings (json_object *root)
{
  gchar *path = gog_config_app_file (SETTINGS_FILE);
  GError *error = NULL;
  if (!g_file_set_contents (path, json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY),
                            -1, &error))
    {
      g_warning ("Could not save GOG download settings: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
}

guint
gog_sync_get_threads (void)
{
  json_object *root = load_settings ();
  gint64 threads = ju_get_int64 (root, "threads", 3);
  json_object_put (root);
  return (guint) CLAMP (threads, 1, 8);
}

void
gog_sync_set_threads (guint threads)
{
  json_object *root = load_settings ();
  json_object_object_add (root, "threads", json_object_new_int64 (CLAMP (threads, 1, 8)));
  save_settings (root);
  json_object_put (root);
}

guint
gog_sync_get_game_languages (const char *gamename)
{
  json_object *root = load_settings ();
  const char *arg = ju_get_string (ju_get_obj (root, "languages"), gamename, NULL);
  /* "all" is stored as such; nothing stored means the default. */
  guint mask = !arg ? GOG_SYNC_LANGUAGES_DEFAULT : languages_from_arg (arg);
  json_object_put (root);
  return mask;
}

void
gog_sync_set_game_languages (const char *gamename, guint mask)
{
  json_object *root = load_settings ();
  json_object *langs = ju_get_obj (root, "languages");
  if (!langs)
    {
      langs = json_object_new_object ();
      json_object_object_add (root, "languages", langs);
    }
  if (mask != GOG_SYNC_LANGUAGES_DEFAULT)
    {
      gchar *arg = languages_arg (mask);
      json_object_object_add (langs, gamename, json_object_new_string (arg));
      g_free (arg);
    }
  else
    json_object_object_del (langs, gamename);
  save_settings (root);
  json_object_put (root);
}

gboolean
gog_sync_get_default_original (void)
{
  json_object *root = load_settings ();
  gboolean original = ju_get_bool (root, "default_original", TRUE);
  json_object_put (root);
  return original;
}

void
gog_sync_set_default_original (gboolean original)
{
  json_object *root = load_settings ();
  json_object_object_add (root, "default_original", json_object_new_boolean (original));
  save_settings (root);
  json_object_put (root);
}

guint
gog_sync_get_default_languages (void)
{
  json_object *root = load_settings ();
  guint mask = languages_from_arg (ju_get_string (root, "default_languages", ""));
  json_object_put (root);
  return mask;
}

void
gog_sync_set_default_languages (guint mask)
{
  json_object *root = load_settings ();
  if (mask)
    {
      gchar *arg = languages_arg (mask);
      json_object_object_add (root, "default_languages", json_object_new_string (arg));
      g_free (arg);
    }
  else
    json_object_object_del (root, "default_languages");
  save_settings (root);
  json_object_put (root);
}

#define LANGUAGE_EN 1u /* lgogdownloader's bit for English */

guint
gog_sync_default_languages_for (const char *gamename, guint offered)
{
  gboolean original = gog_sync_get_default_original ();
  guint extra = gog_sync_get_default_languages ();
  if (!original && !extra)
    return 0; /* nothing narrowed down: all languages */
  guint mask = extra;
  if (original)
    mask |= gog_original_languages (gamename, NULL);
  if (!offered)
    return mask;
  mask &= offered;
  if (!mask)
    mask = offered & LANGUAGE_EN; /* 0, i.e. all, if not even English */
  return mask;
}

gchar *
gog_sync_describe_default_for (const char *gamename, guint offered)
{
  guint mask = gog_sync_default_languages_for (gamename, offered);
  gchar *langs = gog_sync_describe_languages (mask);
  gchar *source = NULL;
  guint original = gog_sync_get_default_original () ? gog_original_languages (gamename, &source)
                                                   : 0;
  gchar *out;
  if (original && (mask & original) && source)
    out = g_strdup_printf ("%s (original: %s)", langs, source);
  else if (gog_sync_get_default_original () && !original)
    out = g_strdup_printf ("%s (original language not known)", langs);
  else
    out = g_strdup (langs);
  g_free (source);
  g_free (langs);
  return out;
}

guint
gog_sync_item_languages (GogSyncItem *item)
{
  guint offered = item->languages_offered;
  guint chosen = gog_sync_get_game_languages (item->gamename);
  /* Not known what the game comes in (its details couldn't be fetched):
   * go by the choice, or the default, as it stands. */
  if (!offered)
    return chosen == GOG_SYNC_LANGUAGES_DEFAULT
               ? gog_sync_default_languages_for (item->gamename, 0)
               : chosen;
  /* With one language there is nothing to choose. */
  if ((offered & (offered - 1)) == 0)
    return 0;
  if (chosen == GOG_SYNC_LANGUAGES_DEFAULT)
    return gog_sync_default_languages_for (item->gamename, offered);
  if (chosen == 0)
    return 0; /* "All languages" */
  /* A chosen language the game no longer has: the default, not everything. */
  return (chosen & offered) ? (chosen & offered)
                            : gog_sync_default_languages_for (item->gamename, offered);
}

gboolean
gog_sync_item_has_wanted_files (GogSyncItem *item)
{
  if (item->files->len == 0)
    return TRUE;
  guint langs = gog_sync_item_languages (item);
  for (guint f = 0; f < item->files->len; f++)
    if (gog_sync_file_wanted (g_ptr_array_index (item->files, f), langs))
      return TRUE;
  return FALSE;
}

void
gog_sync_failure_free (GogSyncFailure *failure)
{
  if (!failure)
    return;
  g_free (failure->gamename);
  g_free (failure->filename);
  g_free (failure->reason);
  g_free (failure);
}

static gboolean
is_ignored (json_object *settings, const char *gamename, const char *filename)
{
  json_object *files = ju_get_array (ju_get_obj (settings, "ignored"), gamename);
  for (size_t i = 0; files && i < json_object_array_length (files); i++)
    if (g_strcmp0 (json_object_get_string (json_object_array_get_idx (files, i)), filename) == 0)
      return TRUE;
  return FALSE;
}

/* Drops ignored files, and games left with none. */
static void
remove_ignored (GPtrArray *items)
{
  json_object *settings = load_settings ();
  for (guint i = items->len; i > 0; i--)
    {
      GogSyncItem *item = g_ptr_array_index (items, i - 1);
      guint before = item->files->len;
      for (guint f = item->files->len; f > 0; f--)
        if (is_ignored (settings, item->gamename,
                        ((GogSyncFile *) g_ptr_array_index (item->files, f - 1))->filename))
          g_ptr_array_remove_index (item->files, f - 1);
      if (item->files->len < before)
        {
          item->n_files = item->files->len;
          if (item->n_files == 0)
            g_ptr_array_remove_index (items, i - 1);
        }
    }
  json_object_put (settings);
}

void
gog_sync_ignore_files (GPtrArray *failures)
{
  json_object *settings = load_settings ();
  json_object *ignored = ju_get_obj (settings, "ignored");
  if (!ignored)
    {
      ignored = json_object_new_object ();
      json_object_object_add (settings, "ignored", ignored);
    }
  for (guint i = 0; i < failures->len; i++)
    {
      GogSyncFailure *failure = g_ptr_array_index (failures, i);
      if (is_ignored (settings, failure->gamename, failure->filename))
        continue;
      json_object *files = ju_get_array (ignored, failure->gamename);
      if (!files)
        {
          files = json_object_new_array ();
          json_object_object_add (ignored, failure->gamename, files);
        }
      json_object_array_add (files, json_object_new_string (failure->filename));
    }
  save_settings (settings);
  json_object_put (settings);

  gint64 checked = 0;
  GPtrArray *saved = gog_sync_load_results (&checked);
  remove_ignored (saved);
  save_results_checked (saved, checked);
  g_ptr_array_unref (saved);
}

/* ---- the check's results ---- */

static gboolean
game_folder_exists (const char *library_dir, const char *gamename)
{
  gchar *path = g_build_filename (library_dir ? library_dir : ".", gamename, NULL);
  gboolean exists = g_file_test (path, G_FILE_TEST_IS_DIR);
  g_free (path);
  return exists;
}

static gint
compare_items (gconstpointer a, gconstpointer b)
{
  const GogSyncItem *ia = *(const GogSyncItem *const *) a;
  const GogSyncItem *ib = *(const GogSyncItem *const *) b;
  return g_strcmp0 (ia->gamename, ib->gamename);
}

static gboolean
is_all (const char *s, gboolean (*pred) (gchar), gsize min_len)
{
  gsize n = 0;
  for (; *s; s++, n++)
    if (!pred (*s))
      return FALSE;
  return n >= min_len;
}

/* TRUE if s is one or more digits followed by end. */
static gboolean
is_all_digits_until (const char *s, char end)
{
  const char *p = s;
  while (g_ascii_isdigit (*p))
    p++;
  return p > s && *p == end && p[1] == '\0';
}

static gboolean
is_digit (gchar c)
{
  return g_ascii_isdigit (c);
}

static gboolean
is_xdigit (gchar c)
{
  return g_ascii_isxdigit (c);
}

/* "<file>[ <size>[ <hash>]]" -> "<file>"; only ND lines have no size. */
static gchar *
status_filename (const char *code, const char *rest)
{
  gchar *name = g_strdup (rest);
  if (g_strcmp0 (code, "ND") == 0)
    return name;
  gchar *space = strrchr (name, ' ');
  if (space && is_all (space + 1, is_xdigit, 32) && strlen (space + 1) == 32)
    {
      *space = '\0';
      space = strrchr (name, ' ');
    }
  if (space && is_all (space + 1, is_digit, 1))
    *space = '\0';
  return name;
}

GPtrArray *
gog_sync_parse_status (const char *output, const char *library_dir,
                       const char *const *base_names)
{
  gchar *current_game = NULL;
  GHashTable *by_name = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);

  gchar **lines = g_strsplit (output ? output : "", "\n", -1);
  for (gchar **line = lines; *line; line++)
    {
      gchar **tok = g_strsplit (g_strstrip (*line), " ", 3);
      gboolean is_status = g_strv_length (tok) == 3 &&
                           (g_strcmp0 (tok[0], "OK") == 0 || g_strcmp0 (tok[0], "ND") == 0 ||
                            g_strcmp0 (tok[0], "FS") == 0 || g_strcmp0 (tok[0], "MD5") == 0);
      const char *game = NULL;
      if (is_status)
        {
          if (!base_names || g_strv_contains (base_names, tok[1]) || !current_game)
            {
              g_free (current_game);
              current_game = g_strdup (tok[1]);
            }
          game = current_game; /* a DLC name counts under its game */
        }
      gboolean wanted = is_status && g_strcmp0 (tok[0], "OK") != 0;
      if (wanted)
        {
          GogSyncItem *item = g_hash_table_lookup (by_name, game);
          if (!item)
            {
              item = item_new (game);
              item->kind = game_folder_exists (library_dir, game) ? GOG_SYNC_UPDATE
                                                                  : GOG_SYNC_NEW_GAME;
              g_ptr_array_add (items, item);
              g_hash_table_insert (by_name, item->gamename, item);
            }
          GogSyncFile *file = g_new0 (GogSyncFile, 1);
          file->gamename = g_strdup (tok[1]);
          file->filename = status_filename (tok[0], tok[2]);
          file->part = g_strcmp0 (tok[1], game) == 0 ? GOG_SYNC_PART_GAME : GOG_SYNC_PART_DLC;
          g_ptr_array_add (item->files, file);
          item->n_files++;
        }
      g_strfreev (tok);
    }
  g_strfreev (lines);

  g_hash_table_unref (by_name);
  g_free (current_game);
  g_ptr_array_sort (items, compare_items);
  return items;
}

typedef struct
{
  GogSyncPart part;
  guint languages;
  guint64 size;
  const char *label;
  const char *dlc_title;
} FileDetails;

static void
add_details (GHashTable *table, json_object *product, gboolean is_dlc, guint *offered)
{
  static const char *const lists[] = { "installers", "extras", "patches", "languagepacks" };
  for (guint l = 0; l < G_N_ELEMENTS (lists); l++)
    {
      json_object *arr = ju_get_array (product, lists[l]);
      size_t n = arr ? json_object_array_length (arr) : 0;
      for (size_t i = 0; i < n; i++)
        {
          json_object *f = json_object_array_get_idx (arr, i);
          const char *path = ju_get_string (f, "path", NULL);
          const char *gamename = ju_get_string (f, "gamename", NULL);
          if (!path || !gamename)
            continue;
          gint64 type = ju_get_int64 (f, "type", 0);
          FileDetails *d = g_new0 (FileDetails, 1);
          /* GFTYPE_*: base 1 2 4 8, DLC 16 32 64 128 */
          d->part = (type & (2 | 32)) || l == 1 ? GOG_SYNC_PART_EXTRA
                    : (type & (16 | 64 | 128)) || is_dlc ? GOG_SYNC_PART_DLC
                                                         : GOG_SYNC_PART_GAME;
          d->languages = (guint) ju_get_int64 (f, "language", 0);
          const char *size = ju_get_string (f, "size", NULL);
          d->size = size ? g_ascii_strtoull (size, NULL, 10) : (guint64) ju_get_int64 (f, "size", 0);
          d->label = ju_get_string (f, "name", NULL);
          d->dlc_title = is_dlc ? ju_get_string (product, "title", NULL) : NULL;
          if (l == 0 || l == 3)
            *offered |= d->languages;
          const char *slash = strrchr (path, '/');
          g_hash_table_insert (table, g_strdup_printf ("%s/%s", gamename, slash ? slash + 1 : path),
                               d);
        }
    }
}

void
gog_sync_apply_details (GPtrArray *items, const char *list_output)
{
  /* The JSON starts at the first line opening an array. */
  const char *start = list_output;
  while (start && *start != '[')
    {
      start = strchr (start, '\n');
      if (start)
        start++;
    }
  if (!start)
    return;
  json_tokener *tokener = json_tokener_new ();
  json_object *root = json_tokener_parse_ex (tokener, start, (int) strlen (start));
  json_tokener_free (tokener);
  if (!root || !json_object_is_type (root, json_type_array))
    {
      if (root)
        json_object_put (root);
      return;
    }

  GHashTable *offered = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  for (size_t i = 0; i < json_object_array_length (root); i++)
    {
      json_object *game = json_object_array_get_idx (root, i);
      const char *gamename = ju_get_string (game, "gamename", NULL);
      if (!gamename)
        continue;
      guint langs = 0;
      add_details (table, game, FALSE, &langs);
      json_object *dlcs = ju_get_array (game, "dlcs");
      for (size_t d = 0; dlcs && d < json_object_array_length (dlcs); d++)
        add_details (table, json_object_array_get_idx (dlcs, d), TRUE, &langs);
      g_hash_table_insert (offered, g_strdup (gamename), GUINT_TO_POINTER (langs));
    }

  for (guint i = 0; items && i < items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (items, i);
      item->languages_offered |= GPOINTER_TO_UINT (g_hash_table_lookup (offered, item->gamename));
      for (guint f = 0; f < item->files->len; f++)
        {
          GogSyncFile *file = g_ptr_array_index (item->files, f);
          gchar *key = g_strdup_printf ("%s/%s", file->gamename, file->filename);
          FileDetails *d = g_hash_table_lookup (table, key);
          g_free (key);
          if (!d)
            continue;
          file->part = d->part;
          file->languages = d->languages;
          file->size = d->size;
          g_free (file->label);
          file->label = g_strdup (d->label);
          g_free (file->dlc_title);
          file->dlc_title = g_strdup (d->dlc_title);
        }
    }
  /* The table's strings point into root. */
  g_hash_table_unref (table);
  g_hash_table_unref (offered);
  json_object_put (root);
}

/* ---- remembered results ---- */

GPtrArray *
gog_sync_load_results (gint64 *out_checked_unix)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);
  if (out_checked_unix)
    *out_checked_unix = 0;
  gchar *path = gog_config_app_file (RESULTS_FILE);
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (!root)
    return items;

  if (out_checked_unix)
    *out_checked_unix = ju_get_int64 (root, "checked", 0);
  json_object *arr = ju_get_array (root, "items");
  size_t n = arr ? json_object_array_length (arr) : 0;
  for (size_t i = 0; i < n; i++)
    {
      json_object *rec = json_object_array_get_idx (arr, i);
      const char *name = ju_get_string (rec, "gamename", NULL);
      if (!name)
        continue;
      GogSyncItem *item = item_new (name);
      item->kind = g_strcmp0 (ju_get_string (rec, "kind", ""), "new") == 0 ? GOG_SYNC_NEW_GAME
                                                                          : GOG_SYNC_UPDATE;
      item->n_files = (guint) ju_get_int64 (rec, "files", 0);
      item->languages_offered = (guint) ju_get_int64 (rec, "languages", 0);
      json_object *files = ju_get_array (rec, "file_list");
      for (size_t f = 0; files && f < json_object_array_length (files); f++)
        {
          json_object *jf = json_object_array_get_idx (files, f);
          const char *fname = ju_get_string (jf, "filename", NULL);
          if (!fname)
            continue;
          GogSyncFile *file = g_new0 (GogSyncFile, 1);
          file->gamename = g_strdup (ju_get_string (jf, "gamename", name));
          file->filename = g_strdup (fname);
          file->label = g_strdup (ju_get_string (jf, "label", NULL));
          file->dlc_title = g_strdup (ju_get_string (jf, "dlc", NULL));
          gint64 part = ju_get_int64 (jf, "part", GOG_SYNC_PART_GAME);
          file->part = part == GOG_SYNC_PART_DLC || part == GOG_SYNC_PART_EXTRA ? (GogSyncPart) part
                                                                                : GOG_SYNC_PART_GAME;
          file->languages = (guint) ju_get_int64 (jf, "languages", 0);
          file->size = (guint64) ju_get_int64 (jf, "size", 0);
          g_ptr_array_add (item->files, file);
        }
      g_ptr_array_add (items, item);
    }
  json_object_put (root);
  return items;
}

static void
save_results_checked (GPtrArray *items, gint64 checked_unix)
{
  json_object *root = json_object_new_object ();
  json_object_object_add (root, "checked", json_object_new_int64 (checked_unix));
  json_object *arr = json_object_new_array ();
  for (guint i = 0; items && i < items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (items, i);
      json_object *rec = json_object_new_object ();
      json_object_object_add (rec, "gamename", json_object_new_string (item->gamename));
      json_object_object_add (rec, "kind",
                              json_object_new_string (item->kind == GOG_SYNC_NEW_GAME ? "new"
                                                                                       : "update"));
      json_object_object_add (rec, "files", json_object_new_int64 (item->n_files));
      json_object_object_add (rec, "languages", json_object_new_int64 (item->languages_offered));
      json_object *files = json_object_new_array ();
      for (guint f = 0; f < item->files->len; f++)
        {
          GogSyncFile *file = g_ptr_array_index (item->files, f);
          json_object *jf = json_object_new_object ();
          json_object_object_add (jf, "gamename", json_object_new_string (file->gamename));
          json_object_object_add (jf, "filename", json_object_new_string (file->filename));
          if (file->label)
            json_object_object_add (jf, "label", json_object_new_string (file->label));
          if (file->dlc_title)
            json_object_object_add (jf, "dlc", json_object_new_string (file->dlc_title));
          json_object_object_add (jf, "part", json_object_new_int64 (file->part));
          json_object_object_add (jf, "languages", json_object_new_int64 (file->languages));
          json_object_object_add (jf, "size", json_object_new_int64 ((gint64) file->size));
          json_object_array_add (files, jf);
        }
      json_object_object_add (rec, "file_list", files);
      json_object_array_add (arr, rec);
    }
  json_object_object_add (root, "items", arr);

  gchar *path = gog_config_app_file (RESULTS_FILE);
  GError *error = NULL;
  if (!g_file_set_contents (path, json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY),
                            -1, &error))
    {
      g_warning ("Could not save GOG update results: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
  json_object_put (root);
}

void
gog_sync_save_results (GPtrArray *items)
{
  save_results_checked (items, g_get_real_time () / G_USEC_PER_SEC);
}

/* ---- the job ---- */

typedef enum
{
  JOB_CHECK,
  JOB_DOWNLOAD,
} JobKind;

struct _GogSyncJob
{
  GObject parent_instance;

  JobKind kind;
  gchar *library_dir;
  GArray *requests;   /* GogSyncRequest with owned gamenames; download only */
  guint threads;
  guint64 total_bytes; /* of the files the requests cover */
  guint total_files;
  GHashTable *sizes;  /* filename -> guint64* */
  GHashTable *file_games; /* filename -> the item's gamename */
  GPtrArray *failures; /* GogSyncFailure*, after a download */
  GPtrArray *items;   /* check result */
  GStrv only_games;   /* check only these, NULL for all */

  GThread *thread;
  GMutex proc_mutex;
  GSubprocess *current_proc; /* protected by proc_mutex */
  volatile gboolean cancelled;
};

enum
{
  SIG_PROGRESS,
  SIG_LOG_LINE,
  SIG_FILE_PROGRESS,
  SIG_FINISHED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GogSyncJob, gog_sync_job, G_TYPE_OBJECT)

static void
gog_sync_job_finalize (GObject *object)
{
  GogSyncJob *self = GOG_SYNC_JOB (object);
  g_mutex_clear (&self->proc_mutex);
  if (self->thread)
    g_thread_unref (self->thread);
  g_free (self->library_dir);
  g_strfreev (self->only_games);
  if (self->requests)
    {
      for (guint i = 0; i < self->requests->len; i++)
        g_free ((gchar *) g_array_index (self->requests, GogSyncRequest, i).gamename);
      g_array_unref (self->requests);
    }
  if (self->sizes)
    g_hash_table_unref (self->sizes);
  if (self->file_games)
    g_hash_table_unref (self->file_games);
  if (self->failures)
    g_ptr_array_unref (self->failures);
  if (self->items)
    g_ptr_array_unref (self->items);
  G_OBJECT_CLASS (gog_sync_job_parent_class)->finalize (object);
}

static void
gog_sync_job_class_init (GogSyncJobClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_sync_job_finalize;

  signals[SIG_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_DOUBLE,
                                        G_TYPE_STRING);
  signals[SIG_LOG_LINE] = g_signal_new ("log-line", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_FILE_PROGRESS] = g_signal_new ("file-progress", G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                             4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_DOUBLE,
                                             G_TYPE_STRING);
  signals[SIG_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_BOOLEAN,
                                        G_TYPE_STRING);
}

static void
gog_sync_job_init (GogSyncJob *self)
{
  g_mutex_init (&self->proc_mutex);
}

GogSyncJob *
gog_sync_job_new_check (const char *library_dir)
{
  GogSyncJob *self = g_object_new (GOG_TYPE_SYNC_JOB, NULL);
  self->kind = JOB_CHECK;
  self->library_dir = g_strdup (library_dir);
  return self;
}

GogSyncJob *
gog_sync_job_new_check_games (const char *library_dir, const char *const *gamenames)
{
  GogSyncJob *self = gog_sync_job_new_check (library_dir);
  self->only_games = g_strdupv ((gchar **) gamenames);
  return self;
}

GogSyncJob *
gog_sync_job_new_download (const char *library_dir, const GogSyncRequest *requests,
                           guint n_requests, guint threads)
{
  GogSyncJob *self = g_object_new (GOG_TYPE_SYNC_JOB, NULL);
  self->kind = JOB_DOWNLOAD;
  self->library_dir = g_strdup (library_dir);
  self->threads = CLAMP (threads, 1, 8);
  self->requests = g_array_new (FALSE, FALSE, sizeof (GogSyncRequest));
  for (guint i = 0; i < n_requests; i++)
    {
      GogSyncRequest r = requests[i];
      r.gamename = g_strdup (r.gamename);
      g_array_append_val (self->requests, r);
    }

  /* What the overall progress is measured against. */
  self->sizes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->file_games = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GPtrArray *saved = gog_sync_load_results (NULL);
  for (guint i = 0; i < saved->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (saved, i);
      for (guint r = 0; r < n_requests; r++)
        {
          if (g_strcmp0 (requests[r].gamename, item->gamename) != 0)
            continue;
          for (guint f = 0; f < item->files->len; f++)
            {
              GogSyncFile *file = g_ptr_array_index (item->files, f);
              if (!(file->part & requests[r].parts) ||
                  !gog_sync_file_wanted (file, requests[r].languages))
                continue;
              self->total_bytes += file->size;
              self->total_files++;
              g_hash_table_insert (self->sizes, g_strdup (file->filename),
                                   g_memdup2 (&file->size, sizeof file->size));
              g_hash_table_insert (self->file_games, g_strdup (file->filename),
                                   g_strdup (item->gamename));
            }
        }
    }
  g_ptr_array_unref (saved);
  return self;
}

GPtrArray *
gog_sync_job_get_items (GogSyncJob *self)
{
  return self->items;
}

GPtrArray *
gog_sync_job_get_failures (GogSyncJob *self)
{
  return self->failures;
}

/* ---- marshalling signals back to the main thread (same pattern as
 * proton_fetch.c) ---- */

typedef struct
{
  GogSyncJob *job;
  guint signal;
  double fraction;
  gboolean success;
  gint slot;
  gchar *text;
  gchar *detail;
} SignalMsg;

static gboolean
emit_idle (gpointer data)
{
  SignalMsg *m = data;
  if (m->signal == SIG_PROGRESS)
    g_signal_emit (m->job, signals[SIG_PROGRESS], 0, m->fraction, m->text);
  else if (m->signal == SIG_LOG_LINE)
    g_signal_emit (m->job, signals[SIG_LOG_LINE], 0, m->text);
  else if (m->signal == SIG_FILE_PROGRESS)
    g_signal_emit (m->job, signals[SIG_FILE_PROGRESS], 0, m->slot, m->text, m->fraction,
                   m->detail);
  else
    g_signal_emit (m->job, signals[SIG_FINISHED], 0, m->success, m->text);
  g_free (m->text);
  g_free (m->detail);
  g_object_unref (m->job);
  g_free (m);
  return G_SOURCE_REMOVE;
}

static void
queue_signal (GogSyncJob *job, guint signal, double fraction, gboolean success, const char *text)
{
  SignalMsg *m = g_new0 (SignalMsg, 1);
  m->job = g_object_ref (job);
  m->signal = signal;
  m->fraction = fraction;
  m->success = success;
  m->text = g_strdup (text);
  g_idle_add (emit_idle, m);
}

static void
job_file_progress (GogSyncJob *job, gint slot, const char *filename, double fraction,
                   const char *detail)
{
  SignalMsg *m = g_new0 (SignalMsg, 1);
  m->job = g_object_ref (job);
  m->signal = SIG_FILE_PROGRESS;
  m->slot = slot;
  m->fraction = fraction;
  m->text = g_strdup (filename);
  m->detail = g_strdup (detail);
  g_idle_add (emit_idle, m);
}

#define job_progress(job, fraction, msg) queue_signal (job, SIG_PROGRESS, fraction, FALSE, msg)
#define job_log(job, line) queue_signal (job, SIG_LOG_LINE, 0, FALSE, line)

/* ---- running lgogdownloader ---- */

/* Called for each output line, ANSI escapes removed. by_cr is TRUE when the
 * line ended in a carriage return, i.e. a progress display that will be
 * overwritten. */
typedef void (*LineFunc) (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data);

static gchar *
strip_ansi (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p; p++)
    {
      if (*p == '\033')
        {
          if (p[1] == '[')
            {
              p += 2;
              while (*p && !g_ascii_isalpha (*p))
                p++;
              if (!*p)
                break;
            }
          else if (p[1] == ']')
            {
              /* OSC (e.g. the terminal progress "ESC ] 9;4;..."), ends in BEL or ESC \ */
              p += 2;
              while (*p && *p != '\a' && !(*p == '\033' && p[1] == '\\'))
                p++;
              if (!*p)
                break;
              if (*p == '\033')
                p++;
            }
          continue;
        }
      g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static void
flush_line (GogSyncJob *job, GString *buf, gboolean by_cr, LineFunc on_line, gpointer data)
{
  gchar *stripped = strip_ansi (buf->str);
  /* File names and messages are not always UTF-8; GTK only takes UTF-8. */
  gchar *clean = g_utf8_make_valid (stripped, -1);
  g_free (stripped);
  g_strstrip (clean);
  if (*clean)
    on_line (job, clean, by_cr, data);
  g_free (clean);
  g_string_truncate (buf, 0);
}

static gboolean
run_streaming (GogSyncJob *job, gchar **argv, LineFunc on_line, gpointer data, GError **error)
{
  if (job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }

  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                             G_SUBPROCESS_FLAGS_STDERR_MERGE);
  /* Plain output: no colours or progress escapes beyond what we strip. */
  g_subprocess_launcher_setenv (launcher, "TERM", "dumb", TRUE);
  GSubprocess *proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, error);
  g_object_unref (launcher);
  if (!proc)
    return FALSE;

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = proc;
  if (job->cancelled)
    g_subprocess_force_exit (proc);
  g_mutex_unlock (&job->proc_mutex);

  GInputStream *out = g_subprocess_get_stdout_pipe (proc);
  GString *buf = g_string_new (NULL);
  gchar chunk[4096];
  gssize n;
  while ((n = g_input_stream_read (out, chunk, sizeof chunk, NULL, NULL)) > 0)
    for (gssize i = 0; i < n; i++)
      {
        if (chunk[i] == '\n' || chunk[i] == '\r')
          flush_line (job, buf, chunk[i] == '\r', on_line, data);
        else
          g_string_append_c (buf, chunk[i]);
      }
  flush_line (job, buf, FALSE, on_line, data);
  g_string_free (buf, TRUE);

  gboolean ok = g_subprocess_wait (proc, NULL, error);

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = NULL;
  g_mutex_unlock (&job->proc_mutex);

  if (ok && job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ok = FALSE;
    }
  else if (ok && !g_subprocess_get_successful (proc))
    {
      ok = FALSE;
      if (g_subprocess_get_if_signaled (proc))
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "lgogdownloader was stopped (signal %d)",
                     g_subprocess_get_term_sig (proc));
      else
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                     "lgogdownloader exited with status %d", g_subprocess_get_exit_status (proc));
    }

  g_object_unref (proc);
  return ok;
}

/* "Getting game info 12 / 395" -> 12, 395 */
static gboolean
parse_counter (const char *line, const char *prefix, guint *done, guint *total)
{
  const char *p = strstr (line, prefix);
  if (!p)
    return FALSE;
  return sscanf (p + strlen (prefix), " %u / %u", done, total) == 2 && *total > 0;
}

static void
collect_line (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data)
{
  GString *collected = data;
  guint done, total;
  if (parse_counter (line, "Getting game info", &done, &total))
    {
      gchar *msg = g_strdup_printf ("Checking game %u of %u…", done, total);
      job_progress (job, (double) done / total, msg);
      g_free (msg);
    }
  else if (parse_counter (line, "Getting game names", &done, &total) ||
           parse_counter (line, "Getting product data", &done, &total))
    job_progress (job, -1, "Getting your list of games from GOG…");
  else if (!by_cr)
    {
      g_string_append (collected, line);
      g_string_append_c (collected, '\n');
    }
}

/* lgogdownloader redraws its download display with fresh lines: per
 * thread "#0 <file>" (or "#0: Finished") and " 45% ▕███▏ 12.34/100.00MB
 * @ 2.10MB/s ETA: 40s", then "Total: … | Remaining: …". Those go to the
 * progress bars; the log keeps the lasting messages. */
typedef struct
{
  gchar *file;
  guint64 done;  /* bytes, from the progress line */
  guint64 total;
  gint64 last_emit;
} Slot;

typedef struct
{
  Slot slots[MAX_SLOTS];
  gint current;           /* the slot the next progress line belongs to */
  guint64 completed_bytes;
  guint completed_files;
  GHashTable *completed;  /* filenames done in this job */
  GHashTable *failed;     /* filename -> why it failed */
  gint64 last_overall;    /* monotonic µs; the display redraws far faster than needed */
} DownloadState;

/* "HTTP response code said error (404)" -> "not found on GOG's server
 * (HTTP 404)"; curl errors without a code are passed on as they are. */
static gchar *
failure_reason (const char *curl_error)
{
  gchar *text = g_strdup (curl_error);
  guint code = 0;
  gsize len = strlen (text);
  if (len > 2 && text[len - 1] == ')')
    {
      gchar *open = strrchr (text, '(');
      if (open && is_all_digits_until (open + 1, ')'))
        {
          code = (guint) g_ascii_strtoull (open + 1, NULL, 10);
          while (open > text && open[-1] == ' ')
            open--;
          *open = '\0';
        }
    }
  gchar *reason;
  if (code == 404)
    reason = g_strdup ("not found on GOG's server (HTTP 404)");
  else if (code == 401 || code == 403)
    reason = g_strdup_printf ("GOG refused access (HTTP %u); check the login with "
                              "\"lgogdownloader --login\"", code);
  else if (code == 429)
    reason = g_strdup ("GOG asked for fewer requests (HTTP 429)");
  else if (code >= 500 && code < 600)
    reason = g_strdup_printf ("GOG's server had a problem (HTTP %u)", code);
  else if (code)
    reason = g_strdup_printf ("%s (HTTP %u)", text, code);
  else
    reason = g_strdup (*text ? text : "unknown error");
  g_free (text);
  return reason;
}

static void
set_slot (GogSyncJob *job, DownloadState *state, gint i, const char *file)
{
  Slot *slot = &state->slots[i];
  if (g_strcmp0 (slot->file, file) == 0)
    return;
  g_free (slot->file);
  slot->file = g_strdup (file);
  slot->done = slot->total = 0;
  slot->last_emit = 0;
  job_file_progress (job, i, file, 0, NULL);
}

static void
clear_slots (GogSyncJob *job, DownloadState *state)
{
  for (gint i = 0; i < MAX_SLOTS; i++)
    if (state->slots[i].file)
      set_slot (job, state, i, NULL);
}

/* "12.34/100.00MiB" -> bytes; the unit belongs to both numbers. */
static gboolean
parse_sizes (const char *s, guint64 *done, guint64 *total)
{
  /* Not sscanf: its %lf follows the locale, and lgogdownloader prints "12.34". */
  char *end = NULL;
  double d = g_ascii_strtod (s, &end);
  if (!end || *end != '/')
    return FALSE;
  double t = g_ascii_strtod (end + 1, &end);
  const char *unit = end;
  double mult = 1;
  const char *prefixes = "kMGT";
  for (int k = 0; prefixes[k]; k++)
    if (g_ascii_tolower (unit[0]) == g_ascii_tolower (prefixes[k]))
      for (int j = 0; j <= k; j++)
        mult *= strchr (unit, 'i') ? 1024 : 1000;
  *done = (guint64) (d * mult);
  *total = (guint64) (t * mult);
  return TRUE;
}

static void
emit_overall (GogSyncJob *job, DownloadState *state, gboolean force)
{
  gint64 now = g_get_monotonic_time ();
  if (!force && now - state->last_overall < 200 * 1000)
    return;
  state->last_overall = now;
  guint64 bytes = state->completed_bytes;
  for (gint i = 0; i < MAX_SLOTS; i++)
    if (state->slots[i].file)
      bytes += state->slots[i].done;
  guint64 total = MAX (job->total_bytes, bytes);
  gchar *have = g_format_size (bytes);
  gchar *of = g_format_size (total);
  guint files_total = MAX (job->total_files, state->completed_files);
  gchar *msg = g_strdup_printf ("%.0f%% — %s of %s, %u of %u files done",
                                total ? 100.0 * bytes / total : 0, have, of,
                                state->completed_files, files_total);
  job_progress (job, total ? (double) bytes / total : -1, msg);
  g_free (msg);
  g_free (of);
  g_free (have);
}

static void
log_line (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data)
{
  DownloadState *state = data;
  guint done, total;
  if (parse_counter (line, "Getting game info", &done, &total))
    {
      gchar *msg = g_strdup_printf ("Getting game details… %u of %u", done, total);
      job_progress (job, (double) done / total, msg);
      g_free (msg);
      return;
    }
  if (strstr (line, "Getting game names") || strstr (line, "Getting product data"))
    {
      job_progress (job, -1, "Getting your list of games from GOG…");
      return;
    }
  if (line[0] == '#' && g_ascii_isdigit (line[1]))
    {
      char *end = NULL;
      guint64 i = g_ascii_strtoull (line + 1, &end, 10);
      if (i < MAX_SLOTS && end)
        {
          state->current = (gint) i;
          if (*end == ':')
            set_slot (job, state, (gint) i, NULL); /* "#0: Finished" */
          else if (*end == ' ' && !g_hash_table_contains (state->completed, end + 1))
            set_slot (job, state, (gint) i, end + 1);
        }
      return;
    }
  if (g_ascii_isdigit (line[0]))
    {
      char *end = NULL;
      guint64 percent = g_ascii_strtoull (line, &end, 10);
      if (end && *end == '%')
        {
          Slot *slot = state->current >= 0 ? &state->slots[state->current] : NULL;
          if (!slot || !slot->file)
            return;
          /* The sizes follow the bar: the first "<digits>/" after the %. */
          const char *detail = NULL;
          for (const char *p = end + 1; *p && !detail; p++)
            if (*p == '/' && p > end + 1 && g_ascii_isdigit (p[-1]))
              {
                const char *q = p;
                while (q > end + 1 && (g_ascii_isdigit (q[-1]) || q[-1] == '.'))
                  q--;
                detail = q;
              }
          if (detail)
            parse_sizes (detail, &slot->done, &slot->total);
          gint64 now = g_get_monotonic_time ();
          if (now - slot->last_emit >= 100 * 1000)
            {
              slot->last_emit = now;
              job_file_progress (job, state->current, slot->file, MIN (percent, 100) / 100.0,
                                 detail);
            }
          emit_overall (job, state, FALSE);
          return;
        }
    }
  if (g_str_has_prefix (line, "Total:") || g_str_has_prefix (line, "Remaining:") || by_cr)
    return;

  /* A failed file: "Download complete (<curl error>[ (<HTTP code>)]): <file>". */
  const char *failed = strstr (line, "Download complete (");
  if (failed)
    {
      const char *err = failed + strlen ("Download complete (");
      const char *sep = strstr (err, "): ");
      if (sep)
        {
          gchar *curl_error = g_strndup (err, sep - err);
          const char *file = sep + 3;
          gchar *reason = failure_reason (curl_error);
          gchar *out = g_strdup_printf ("Could not download %s: %s", file, reason);
          job_log (job, out);
          g_free (out);
          g_hash_table_replace (state->failed, g_strdup (file), reason);
          for (gint i = 0; i < MAX_SLOTS; i++)
            if (g_strcmp0 (state->slots[i].file, file) == 0)
              set_slot (job, state, i, NULL);
          g_free (curl_error);
          return;
        }
    }
  if (g_str_has_prefix (line, "Failed to open ") || g_str_has_prefix (line, "Failed to create "))
    {
      gchar *name = g_path_get_basename (strchr (line + strlen ("Failed to "), ' ') + 1);
      g_hash_table_replace (state->failed, name, g_strdup ("it could not be saved in the library folder"));
    }

  const char *complete = strstr (line, "Download complete: ");
  const char *deleting = g_str_has_prefix (line, "Deleting ") ? line + strlen ("Deleting ") : NULL;
  if (complete)
    {
      gchar *msg = g_strdup (complete + strlen ("Download complete: "));
      gchar *speed = strstr (msg, " (@");
      if (speed)
        *speed = '\0';
      gchar *out = g_strdup_printf ("Downloaded %s", msg);
      job_log (job, out);
      g_free (out);
      /* "Download complete (<curl error>): file" is a failure and doesn't
       * match the prefix, so this one is done. */
      if (!g_hash_table_contains (state->completed, msg))
        {
          const guint64 *size = g_hash_table_lookup (job->sizes, msg);
          guint64 bytes = size ? *size : 0;
          for (gint i = 0; i < MAX_SLOTS; i++)
            if (g_strcmp0 (state->slots[i].file, msg) == 0)
              {
                if (!size)
                  bytes = state->slots[i].total;
                set_slot (job, state, i, NULL);
              }
          state->completed_bytes += bytes;
          state->completed_files++;
          g_hash_table_add (state->completed, g_strdup (msg));
          emit_overall (job, state, TRUE);
        }
      g_free (msg);
    }
  else if (deleting)
    {
      gchar *name = g_path_get_basename (deleting);
      gchar *out = g_strdup_printf ("Deleted old version: %s", name);
      job_log (job, out);
      g_free (out);
      g_free (name);
    }
  else
    job_log (job, line);
}

static gboolean
check_login (GogSyncJob *job, GError **error)
{
  gchar *argv[] = { LGOG, "--check-login-status", NULL };
  GString *out = g_string_new (NULL);
  gboolean ok = run_streaming (job, argv, collect_line, out, error);
  gboolean logged_in = ok && strstr (out->str, "Login status: Logged in") != NULL;
  g_string_free (out, TRUE);
  if (ok && !logged_in)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "lgogdownloader is not logged in to GOG. Open a terminal, run "
                           "\"lgogdownloader --login\" and try again.");
      return FALSE;
    }
  return ok;
}

/* "^(a|b)$" for --game. Transfer full. */
static gchar *
names_regex (const char *const *names)
{
  GString *regex = g_string_new ("^(");
  for (const char *const *name = names; *name; name++)
    {
      gchar *escaped = g_regex_escape_string (*name, -1);
      g_string_append_printf (regex, "%s%s", name == names ? "" : "|", escaped);
      g_free (escaped);
    }
  g_string_append (regex, ")$");
  return g_string_free (regex, FALSE);
}

static gboolean
check_sync (GogSyncJob *job, GError **error)
{
  job_progress (job, -1, "Checking the GOG login…");
  if (!check_login (job, error))
    return FALSE;

  gchar **base_names;
  gchar *only_regex = NULL;
  if (job->only_games)
    {
      /* The status lists only these games and their DLC. */
      base_names = g_strdupv (job->only_games);
      only_regex = names_regex ((const char *const *) job->only_games);
    }
  else
    {
      job_progress (job, -1, "Getting your list of games from GOG…");
      gchar *list_argv[] = { LGOG, "--list", "games", NULL };
      GString *names_out = g_string_new (NULL);
      if (!run_streaming (job, list_argv, collect_line, names_out, error))
        {
          g_string_free (names_out, TRUE);
          return FALSE;
        }
      base_names = g_strsplit (g_strstrip (names_out->str), "\n", -1);
      g_string_free (names_out, TRUE);
    }

  gchar *argv[] = { LGOG, "--status", "--size-only", "--exclude", "patches",
                    "--directory", job->library_dir, only_regex ? "--game" : NULL, only_regex,
                    NULL };
  GString *out = g_string_new (NULL);
  gboolean ok = run_streaming (job, argv, collect_line, out, error);
  if (ok)
    job->items = gog_sync_parse_status (out->str, job->library_dir,
                                        (const char *const *) base_names);
  g_string_free (out, TRUE);
  g_strfreev (base_names);
  g_free (only_regex);

  if (ok && job->items->len > 0)
    {
      /* What each file is (game, DLC or extra), its language and size. */
      job_progress (job, -1, "Getting details of the games to download…");
      GStrvBuilder *names = g_strv_builder_new ();
      for (guint i = 0; i < job->items->len; i++)
        g_strv_builder_add (names, ((GogSyncItem *) g_ptr_array_index (job->items, i))->gamename);
      GStrv strv = g_strv_builder_end (names);
      g_strv_builder_unref (names);
      gchar *regex = names_regex ((const char *const *) strv);
      gchar *details_argv[] = { LGOG, "--list", "j", "--game", regex, NULL };
      GString *details = g_string_new (NULL);
      GError *details_error = NULL;
      /* Without details the files still download; they just can't be told
       * apart, so this isn't fatal. */
      if (run_streaming (job, details_argv, collect_line, details, &details_error))
        gog_sync_apply_details (job->items, details->str);
      else if (job->cancelled)
        {
          g_propagate_error (error, details_error);
          details_error = NULL;
          ok = FALSE;
        }
      else
        job_log (job, details_error->message);
      g_clear_error (&details_error);
      g_string_free (details, TRUE);
      g_free (regex);
      /* Each game's original language, for the default download languages.
       * Without it a game falls back to English, so a failure isn't fatal. */
      if (ok && !job->cancelled)
        {
          job_progress (job, -1, "Looking up the games' original languages…");
          GError *lookup_error = NULL;
          if (!gog_original_lookup ((const char *const *) strv, &job->cancelled, &lookup_error))
            {
              job_log (job, lookup_error->message);
              g_clear_error (&lookup_error);
            }
          if (job->cancelled)
            {
              g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
              ok = FALSE;
            }
        }
      g_strfreev (strv);
    }
  if (ok)
    remove_ignored (job->items);
  gint64 checked = g_get_real_time () / G_USEC_PER_SEC;
  if (ok && job->only_games)
    {
      /* The other games keep what the last full check found, and its time. */
      GPtrArray *saved = gog_sync_load_results (&checked);
      for (guint i = 0; i < saved->len; i++)
        {
          GogSyncItem *item = g_ptr_array_index (saved, i);
          if (!g_strv_contains ((const char *const *) job->only_games, item->gamename))
            g_ptr_array_add (job->items, g_ptr_array_steal_index (saved, i--));
        }
      g_ptr_array_unref (saved);
      g_ptr_array_sort (job->items, compare_items);
    }
  if (ok)
    save_results_checked (job->items, checked);
  return ok;
}

/* --include for parts. Patches stay excluded, as in the library. */
static gchar *
parts_include (GogSyncPart parts)
{
  GString *out = g_string_new (NULL);
  if (parts & GOG_SYNC_PART_GAME)
    g_string_append (out, "bi,bl,");
  if (parts & GOG_SYNC_PART_DLC)
    g_string_append (out, "di,dl,");
  if (parts & GOG_SYNC_PART_EXTRA)
    g_string_append (out, "be,de,");
  if (out->len)
    g_string_truncate (out, out->len - 1);
  return g_string_free (out, FALSE);
}

/* A file name with its version numbers blanked out: every run of digits
 * and dots that holds a digit becomes '#'. Two versions of the same file
 * ("setup_x_1.2_(500)-1.bin", "setup_x_1.3_(512)-1.bin") then match, while
 * other files (another language, the Mac package) don't. */
gchar *
gog_sync_version_blind_name (const char *name)
{
  GString *out = g_string_new (NULL);
  for (const char *p = name; *p;)
    {
      const char *q = p;
      gboolean digit = FALSE;
      while (*q && (g_ascii_isdigit (*q) || *q == '.'))
        digit |= g_ascii_isdigit (*q++);
      if (q > p && digit)
        {
          g_string_append_c (out, '#');
          p = q;
        }
      else if (q > p)
        {
          g_string_append_len (out, p, q - p);
          p = q;
        }
      else
        g_string_append_c (out, *p++);
    }
  return g_string_free (out, FALSE);
}

/* Deletes the old files listed by --check-orphans that a file downloaded
 * in this run replaces: same folder, same name apart from the version. */
static void
delete_replaced_orphans (GogSyncJob *job, const char *listed, const char *const *games,
                         GHashTable *completed)
{
  gchar **lines = g_strsplit (listed, "\n", -1);
  for (gchar **l = lines; *l; l++)
    {
      const char *path = g_strstrip (*l);
      if (!g_path_is_absolute (path) || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
        continue;
      /* Only inside the folder of a game in this download. */
      gboolean ours = FALSE;
      for (const char *const *g = games; *g && !ours; g++)
        {
          gchar *folder = g_build_filename (job->library_dir, *g, "", NULL);
          ours = g_str_has_prefix (path, folder);
          g_free (folder);
        }
      if (!ours || strstr (path, "/../"))
        continue;

      gchar *dir = g_path_get_dirname (path);
      gchar *name = g_path_get_basename (path);
      gchar *blind = gog_sync_version_blind_name (name);
      const char *replacement = NULL;
      GHashTableIter iter;
      gpointer key;
      g_hash_table_iter_init (&iter, completed);
      while (!replacement && g_hash_table_iter_next (&iter, &key, NULL))
        {
          if (g_strcmp0 (key, name) == 0)
            continue;
          gchar *other = gog_sync_version_blind_name (key);
          gchar *sibling = g_build_filename (dir, key, NULL);
          if (g_strcmp0 (other, blind) == 0 && g_file_test (sibling, G_FILE_TEST_IS_REGULAR))
            replacement = key;
          g_free (sibling);
          g_free (other);
        }
      if (replacement)
        {
          gchar *msg = g_remove (path) == 0
                           ? g_strdup_printf ("Deleted old version: %s", name)
                           : g_strdup_printf ("Could not delete old version %s: %s", name,
                                              g_strerror (errno));
          job_log (job, msg);
          g_free (msg);
        }
      g_free (blind);
      g_free (name);
      g_free (dir);
    }
  g_strfreev (lines);
}

/* Each set of choices (languages and parts) is one lgogdownloader run. */
typedef struct
{
  guint languages;
  GogSyncPart parts;
  GPtrArray *names;
} Batch;

static gboolean
download_sync (GogSyncJob *job, GError **error)
{
  job_progress (job, -1, "Checking the GOG login…");
  if (!check_login (job, error))
    return FALSE;

  GArray *batches = g_array_new (FALSE, TRUE, sizeof (Batch));
  GPtrArray *all_names = g_ptr_array_new ();
  for (guint i = 0; i < job->requests->len; i++)
    {
      GogSyncRequest *r = &g_array_index (job->requests, GogSyncRequest, i);
      if (!r->parts)
        continue;
      g_ptr_array_add (all_names, (gpointer) r->gamename);
      Batch *batch = NULL;
      for (guint b = 0; b < batches->len && !batch; b++)
        {
          Batch *candidate = &g_array_index (batches, Batch, b);
          if (candidate->languages == r->languages && candidate->parts == r->parts)
            batch = candidate;
        }
      if (!batch)
        {
          Batch fresh = { r->languages, r->parts, g_ptr_array_new () };
          g_array_append_val (batches, fresh);
          batch = &g_array_index (batches, Batch, batches->len - 1);
        }
      g_ptr_array_add (batch->names, (gpointer) r->gamename);
    }
  g_ptr_array_add (all_names, NULL);

  DownloadState state = { 0 };
  state.current = -1;
  state.completed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  state.failed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gchar *threads = g_strdup_printf ("%u", job->threads);
  gboolean ok = TRUE;
  job_progress (job, -1, "Starting the download…");
  for (guint b = 0; ok && b < batches->len; b++)
    {
      Batch *batch = &g_array_index (batches, Batch, b);
      g_ptr_array_add (batch->names, NULL);
      gchar *regex = names_regex ((const char *const *) batch->names->pdata);
      gchar *include = parts_include (batch->parts);
      gchar *language = languages_arg (batch->languages);
      /* The options the library is downloaded with. Old versions are
       * deleted afterwards, only those a new download replaced. */
      gchar *argv[] = { LGOG, "--download", "--threads", threads, "--exclude", "patches",
                        "--include", include, "--language", language, "--save-serials",
                        "--save-icon", "--save-logo", "--save-product-json", "--directory",
                        job->library_dir, "--game", regex, NULL };
      ok = run_streaming (job, argv, log_line, &state, error);
      clear_slots (job, &state);
      g_free (language);
      g_free (include);
      g_free (regex);
    }
  /* What failed, and what never came: lgogdownloader skips some files
   * without saying so (e.g. when GOG gives no download link). */
  job->failures = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_failure_free);
  GHashTable *failed_games = g_hash_table_new (g_str_hash, g_str_equal);
  if (!job->cancelled)
    {
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init (&iter, job->sizes);
      while (g_hash_table_iter_next (&iter, &key, &value))
        {
          if (g_hash_table_contains (state.completed, key))
            continue;
          const char *reason = g_hash_table_lookup (state.failed, key);
          if (!reason && !ok)
            continue; /* the run stopped before it got there; the error says why */
          GogSyncFailure *failure = g_new0 (GogSyncFailure, 1);
          failure->gamename = g_strdup (g_hash_table_lookup (job->file_games, key));
          failure->filename = g_strdup (key);
          failure->reason = g_strdup (reason ? reason
                                             : "lgogdownloader skipped it without saying why");
          g_ptr_array_add (job->failures, failure);
          g_hash_table_add (failed_games, failure->gamename);
        }
    }

  /* Old versions. lgogdownloader's --check-orphans lists every file in a
   * game's folder that GOG no longer offers, always over every language
   * and part, whatever was downloaded. Deleting all of them (as
   * --delete-orphans does) would also take the old versions of the DLC,
   * extras and languages left out of this download, with nothing to
   * replace them, and an old version whose download just failed. So only
   * the list is asked for, and an old file goes only when a file of the
   * same kind came in this run, next to it. */
  if (ok && !job->cancelled && g_hash_table_size (state.completed) > 0)
    {
      job_progress (job, -1, "Deleting old versions…");
      gchar *regex = names_regex ((const char *const *) all_names->pdata);
      gchar *argv[] = { LGOG, "--check-orphans", "--exclude", "patches", "--directory",
                        job->library_dir, "--game", regex, NULL };
      GString *listed = g_string_new (NULL);
      ok = run_streaming (job, argv, collect_line, listed, error);
      if (ok)
        delete_replaced_orphans (job, listed->str, (const char *const *) all_names->pdata,
                                 state.completed);
      g_string_free (listed, TRUE);
      g_free (regex);
    }

  /* The files that came are off the list; failed ones and files of other
   * languages stay. */
  GPtrArray *saved = gog_sync_load_results (NULL);
  for (guint i = saved->len; i > 0; i--)
    {
      GogSyncItem *item = g_ptr_array_index (saved, i - 1);
      gboolean requested = FALSE;
      for (guint r = 0; r < job->requests->len; r++)
        requested = requested ||
                    g_strcmp0 (g_array_index (job->requests, GogSyncRequest, r).gamename,
                               item->gamename) == 0;
      if (!requested)
        continue;
      if (item->files->len == 0)
        {
          /* Saved before files were listed: nothing to go by but success. */
          if (ok && !g_hash_table_contains (failed_games, item->gamename))
            g_ptr_array_remove_index (saved, i - 1);
          continue;
        }
      for (guint f = item->files->len; f > 0; f--)
        {
          GogSyncFile *file = g_ptr_array_index (item->files, f - 1);
          if (g_hash_table_contains (state.completed, file->filename))
            g_ptr_array_remove_index (item->files, f - 1);
        }
      item->n_files = item->files->len;
      if (item->n_files == 0)
        g_ptr_array_remove_index (saved, i - 1);
    }
  gint64 checked = 0;
  g_ptr_array_unref (gog_sync_load_results (&checked));
  save_results_checked (saved, checked);
  g_ptr_array_unref (saved);

  g_hash_table_unref (failed_games);
  g_free (threads);
  g_hash_table_unref (state.completed);
  g_hash_table_unref (state.failed);
  for (gint i = 0; i < MAX_SLOTS; i++)
    g_free (state.slots[i].file);
  for (guint b = 0; b < batches->len; b++)
    g_ptr_array_unref (g_array_index (batches, Batch, b).names);
  g_array_unref (batches);
  g_ptr_array_unref (all_names);
  return ok;
}

static gpointer
job_thread_func (gpointer data)
{
  GogSyncJob *job = data;
  GError *error = NULL;
  gboolean ok = job->kind == JOB_CHECK ? check_sync (job, &error) : download_sync (job, &error);
  queue_signal (job, SIG_FINISHED, 0, ok,
                ok ? NULL : (error ? error->message : "lgogdownloader failed"));
  g_clear_error (&error);
  g_object_unref (job); /* matches the ref taken in gog_sync_job_start() */
  return NULL;
}

void
gog_sync_job_start (GogSyncJob *self)
{
  g_object_ref (self);
  self->thread = g_thread_new ("gog-sync", job_thread_func, self);
}

void
gog_sync_job_cancel (GogSyncJob *self)
{
  self->cancelled = TRUE;
  g_mutex_lock (&self->proc_mutex);
  if (self->current_proc)
    g_subprocess_force_exit (self->current_proc);
  g_mutex_unlock (&self->proc_mutex);
}
