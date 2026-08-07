/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_JSON_UTIL_H
#define GOG_JSON_UTIL_H

#include <glib.h>
#include <json-c/json.h>

G_BEGIN_DECLS

/* Load and parse a JSON file. Returns NULL (and sets error) on failure. */
json_object *ju_load_file (const char *path, GError **error);

/* Safe field getters. All tolerate a missing key, a NULL object, or a
 * mismatched JSON type by returning the supplied default. */
const char *ju_get_string (json_object *obj, const char *key, const char *def);
gboolean     ju_get_bool   (json_object *obj, const char *key, gboolean def);
gint64       ju_get_int64  (json_object *obj, const char *key, gint64 def);
json_object *ju_get_obj    (json_object *obj, const char *key);
json_object *ju_get_array  (json_object *obj, const char *key);

/* GOG's "languages" field is a JSON object ({"en":"English",...}) in most
 * product jsons but a plain JSON array (["en", ...]) in some. Returns a
 * newly allocated, NULL-terminated GStrv of the human-readable values (or
 * the raw strings when it's an array), sorted and de-duplicated. */
GStrv ju_get_languages (json_object *root);

G_END_DECLS

#endif
