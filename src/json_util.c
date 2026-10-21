/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "json_util.h"
#include <stdio.h>
#include <string.h>

json_object *
ju_load_file (const char *path, GError **error)
{
  gchar *contents = NULL;
  gsize length = 0;

  if (!g_file_get_contents (path, &contents, &length, error))
    return NULL;

  if (length > INT_MAX)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "JSON file %s is too large (%zu bytes, max %d)", path, length, INT_MAX);
      g_free (contents);
      return NULL;
    }

  /* json-c doesn't check UTF-8, and GLib's UTF-8 functions read past the
   * end of a string that ends in a broken sequence. */
  if (!g_utf8_validate_len (contents, length, NULL))
    {
      gchar *valid = g_utf8_make_valid (contents, (gssize) length);
      g_free (contents);
      contents = valid;
      length = strlen (valid);
    }

  json_tokener *tok = json_tokener_new ();
  json_object *obj = json_tokener_parse_ex (tok, contents, (int) length);
  enum json_tokener_error jerr = json_tokener_get_error (tok);

  if (jerr != json_tokener_success || obj == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "JSON parse error in %s: %s", path,
                   json_tokener_error_desc (jerr));
      if (obj)
        json_object_put (obj);
      obj = NULL;
    }

  json_tokener_free (tok);
  g_free (contents);
  return obj;
}

json_object *
ju_parse_string (const char *text)
{
  if (!text)
    return NULL;
  if (g_utf8_validate (text, -1, NULL))
    return json_tokener_parse (text);
  gchar *valid = g_utf8_make_valid (text, -1);
  json_object *obj = json_tokener_parse (valid);
  g_free (valid);
  return obj;
}

const char *
ju_get_string (json_object *obj, const char *key, const char *def)
{
  json_object *v;
  if (!obj || !json_object_object_get_ex (obj, key, &v))
    return def;
  if (!json_object_is_type (v, json_type_string))
    return def;
  return json_object_get_string (v);
}

gboolean
ju_get_bool (json_object *obj, const char *key, gboolean def)
{
  json_object *v;
  if (!obj || !json_object_object_get_ex (obj, key, &v))
    return def;
  if (!json_object_is_type (v, json_type_boolean))
    return def;
  return json_object_get_boolean (v);
}

gint64
ju_get_int64 (json_object *obj, const char *key, gint64 def)
{
  json_object *v;
  if (!obj || !json_object_object_get_ex (obj, key, &v))
    return def;
  if (!json_object_is_type (v, json_type_int))
    return def;
  return json_object_get_int64 (v);
}

json_object *
ju_get_obj (json_object *obj, const char *key)
{
  json_object *v;
  if (!obj || !json_object_object_get_ex (obj, key, &v))
    return NULL;
  if (!json_object_is_type (v, json_type_object))
    return NULL;
  return v;
}

json_object *
ju_get_array (json_object *obj, const char *key)
{
  json_object *v;
  if (!obj || !json_object_object_get_ex (obj, key, &v))
    return NULL;
  if (!json_object_is_type (v, json_type_array))
    return NULL;
  return v;
}

static int
strcmp_qsort (const void *a, const void *b)
{
  const char *const *sa = a;
  const char *const *sb = b;
  return g_strcmp0 (*sa, *sb);
}

GStrv
ju_get_languages (json_object *root)
{
  json_object *langs;
  GPtrArray *out = g_ptr_array_new ();

  if (!root || !json_object_object_get_ex (root, "languages", &langs))
    {
      g_ptr_array_add (out, NULL);
      return (GStrv) g_ptr_array_free (out, FALSE);
    }

  if (json_object_is_type (langs, json_type_object))
    {
      json_object_object_foreach (langs, key, val)
        {
          (void) key;
          if (json_object_is_type (val, json_type_string))
            g_ptr_array_add (out, g_strdup (json_object_get_string (val)));
        }
    }
  else if (json_object_is_type (langs, json_type_array))
    {
      size_t n = json_object_array_length (langs);
      for (size_t i = 0; i < n; i++)
        {
          json_object *item = json_object_array_get_idx (langs, i);
          if (json_object_is_type (item, json_type_string))
            g_ptr_array_add (out, g_strdup (json_object_get_string (item)));
        }
    }

  /* de-dup */
  qsort (out->pdata, out->len, sizeof (gpointer), strcmp_qsort);
  for (guint i = 0; i + 1 < out->len;)
    {
      if (g_strcmp0 (g_ptr_array_index (out, i), g_ptr_array_index (out, i + 1)) == 0)
        {
          g_free (g_ptr_array_index (out, i + 1));
          g_ptr_array_remove_index (out, i + 1);
        }
      else
        i++;
    }

  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}
