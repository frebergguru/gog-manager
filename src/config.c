/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "config.h"
#include "json_util.h"
#include <json-c/json.h>
#include <errno.h>

/* Directory the app is running from. For an AppImage the executing binary
 * lives inside the mounted image, so the real on-disk location is given by the
 * $APPIMAGE environment variable; otherwise we resolve /proc/self/exe. Falls
 * back to the current working directory if neither is available. */
static gchar *
app_dir (void)
{
  const gchar *appimage = g_getenv ("APPIMAGE");
  if (appimage && *appimage)
    return g_path_get_dirname (appimage);

  gchar *exe = g_file_read_link ("/proc/self/exe", NULL);
  if (exe)
    {
      gchar *dir = g_path_get_dirname (exe);
      g_free (exe);
      return dir;
    }

  return g_get_current_dir ();
}

/* The config lives next to the executable (portable), e.g.
 * .../GOG/Games/gog-manager.cfg, so it travels with the app. */
static gchar *
config_dir (void)
{
  return app_dir ();
}

static gchar *
config_file (void)
{
  gchar *dir = config_dir ();
  gchar *path = g_build_filename (dir, "gog-manager.cfg", NULL);
  g_free (dir);
  return path;
}

/* Per-game install state lives next to the executable too, so a portable copy
 * carries its install records with it. */
static gchar *
state_file (void)
{
  gchar *dir = app_dir ();
  gchar *path = g_build_filename (dir, "gog-manager-state.json", NULL);
  g_free (dir);
  return path;
}

gchar *
gog_config_app_file (const char *name)
{
  gchar *dir = app_dir ();
  gchar *path = g_build_filename (dir, name, NULL);
  g_free (dir);
  return path;
}

GogConfig *
gog_config_load (void)
{
  GogConfig *cfg = g_new0 (GogConfig, 1);
  cfg->default_runner_kind = GOG_RUNNER_NONE;

  gchar *path = config_file ();
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (!root)
    return cfg;

  const char *library_path = ju_get_string (root, "library_path", NULL);
  if (library_path && g_path_is_absolute (library_path))
    cfg->library_path = g_strdup (library_path);

  const char *install_root = ju_get_string (root, "install_root", NULL);
  if (install_root && g_path_is_absolute (install_root))
    cfg->install_root = g_strdup (install_root);
  cfg->default_runner_kind = gog_runner_kind_from_string (ju_get_string (root, "default_runner_kind", ""));
  cfg->default_runner_path = g_strdup (ju_get_string (root, "default_runner_path", NULL));

  cfg->library_view = g_strdup (ju_get_string (root, "library_view", NULL));

  json_object *custom = ju_get_array (root, "custom_runner_paths");
  if (custom)
    {
      size_t n = json_object_array_length (custom);
      GPtrArray *arr = g_ptr_array_new ();
      for (size_t i = 0; i < n; i++)
        {
          json_object *item = json_object_array_get_idx (custom, i);
          if (json_object_is_type (item, json_type_string))
            g_ptr_array_add (arr, g_strdup (json_object_get_string (item)));
        }
      g_ptr_array_add (arr, NULL);
      cfg->custom_runner_paths = (GStrv) g_ptr_array_free (arr, FALSE);
    }

  json_object_put (root);
  return cfg;
}

gboolean
gog_config_save (GogConfig *config, GError **error)
{
  gchar *dir = config_dir ();
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create %s", dir);
      g_free (dir);
      return FALSE;
    }
  g_free (dir);

  json_object *root = json_object_new_object ();
  if (config->library_path)
    json_object_object_add (root, "library_path", json_object_new_string (config->library_path));
  if (config->install_root)
    json_object_object_add (root, "install_root", json_object_new_string (config->install_root));
  json_object_object_add (root, "default_runner_kind",
                           json_object_new_string (gog_runner_kind_to_string (config->default_runner_kind)));
  if (config->default_runner_path)
    json_object_object_add (root, "default_runner_path", json_object_new_string (config->default_runner_path));

  json_object *custom = json_object_new_array ();
  if (config->custom_runner_paths)
    for (GStrv p = config->custom_runner_paths; *p; p++)
      json_object_array_add (custom, json_object_new_string (*p));
  json_object_object_add (root, "custom_runner_paths", custom);
  if (config->library_view)
    json_object_object_add (root, "library_view", json_object_new_string (config->library_view));

  const char *path = json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY);
  gchar *file = config_file ();
  gboolean ok = g_file_set_contents (file, path, -1, error);
  g_free (file);
  json_object_put (root);
  return ok;
}

gchar *
gog_config_effective_library_path (GogConfig *config)
{
  if (config && config->library_path)
    return g_strdup (config->library_path);
  return g_get_current_dir ();
}

gboolean
gog_config_library_path_is_default (GogConfig *config)
{
  return !(config && config->library_path);
}

void
gog_config_free (GogConfig *config)
{
  if (!config)
    return;
  g_free (config->library_path);
  g_free (config->install_root);
  g_free (config->default_runner_path);
  g_strfreev (config->custom_runner_paths);
  g_free (config->library_view);
  g_free (config);
}

/* ---- per-game install state ---- */

struct _GogState
{
  GHashTable *by_id; /* gint64* -> json_object* (borrowed record) owned by root */
  json_object *root; /* {"games": {"<id>": {...}}} */
};

GogState *
gog_state_load (void)
{
  GogState *state = g_new0 (GogState, 1);
  state->by_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  gchar *path = state_file ();
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (root && !json_object_is_type (root, json_type_object))
    {
      g_warning ("Ignoring malformed install state file (top level is not an object)");
      json_object_put (root);
      root = NULL;
    }
  if (!root)
    root = json_object_new_object ();
  state->root = root;

  json_object *games = ju_get_obj (root, "games");
  if (games)
    {
      json_object_object_foreach (games, key, val)
        {
          gint64 *id = g_new (gint64, 1);
          *id = g_ascii_strtoll (key, NULL, 10);
          g_hash_table_insert (state->by_id, id, val);
        }
    }

  return state;
}

gboolean
gog_state_save (GogState *state, GError **error)
{
  gchar *dir = app_dir ();
  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create %s", dir);
      g_free (dir);
      return FALSE;
    }
  g_free (dir);

  const char *json_str = json_object_to_json_string_ext (state->root, JSON_C_TO_STRING_PRETTY);
  gchar *path = state_file ();
  gboolean ok = g_file_set_contents (path, json_str, -1, error);
  g_free (path);
  return ok;
}

void
gog_state_free (GogState *state)
{
  if (!state)
    return;
  g_hash_table_unref (state->by_id);
  if (state->root)
    json_object_put (state->root);
  g_free (state);
}

void
gog_state_apply_to_game (GogState *state, GogGame *game)
{
  gint64 id = gog_game_get_id (game);
  gint64 *id_ptr = g_new (gint64, 1);
  *id_ptr = id;
  json_object *rec = g_hash_table_lookup (state->by_id, id_ptr);
  g_free (id_ptr);
  if (!rec)
    return;

  gog_game_set_installed (game, ju_get_bool (rec, "installed", FALSE));
  gog_game_set_install_path (game, ju_get_string (rec, "install_path", NULL));
  gog_game_set_runner_kind (game, gog_runner_kind_from_string (ju_get_string (rec, "runner_kind", "")));
  gog_game_set_runner_path (game, ju_get_string (rec, "runner_path", NULL));
  gog_game_set_prefix_path (game, ju_get_string (rec, "prefix_path", NULL));
}

void
gog_state_update_from_game (GogState *state, GogGame *game)
{
  gint64 id = gog_game_get_id (game);

  json_object *games = ju_get_obj (state->root, "games");
  if (!games)
    {
      games = json_object_new_object ();
      json_object_object_add (state->root, "games", games);
    }

  json_object *rec = json_object_new_object ();
  json_object_object_add (rec, "installed", json_object_new_boolean (gog_game_get_installed (game)));
  if (gog_game_get_install_path (game))
    json_object_object_add (rec, "install_path", json_object_new_string (gog_game_get_install_path (game)));
  json_object_object_add (rec, "runner_kind",
                           json_object_new_string (gog_runner_kind_to_string (gog_game_get_runner_kind (game))));
  if (gog_game_get_runner_path (game))
    json_object_object_add (rec, "runner_path", json_object_new_string (gog_game_get_runner_path (game)));
  if (gog_game_get_prefix_path (game))
    json_object_object_add (rec, "prefix_path", json_object_new_string (gog_game_get_prefix_path (game)));

  gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, id);
  json_object_object_add (games, key, rec); /* takes ownership, replaces old */
  g_free (key);

  gint64 *hkey = g_new (gint64, 1);
  *hkey = id;
  g_hash_table_insert (state->by_id, hkey, rec);
}

void
gog_state_remove (GogState *state, gint64 id)
{
  json_object *games = ju_get_obj (state->root, "games");
  if (games)
    {
      gchar *key = g_strdup_printf ("%" G_GINT64_FORMAT, id);
      json_object_object_del (games, key);
      g_free (key);
    }
  gint64 *id_ptr = g_new (gint64, 1);
  *id_ptr = id;
  g_hash_table_remove (state->by_id, id_ptr);
  g_free (id_ptr);
}
