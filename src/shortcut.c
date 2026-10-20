/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "shortcut.h"
#include "launch.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

gchar *
gog_shortcut_path (GogGame *game)
{
  gchar *name = g_strdup_printf ("gog-manager-%" G_GINT64_FORMAT ".desktop", gog_game_get_id (game));
  gchar *path = g_build_filename (g_get_user_data_dir (), "applications", name, NULL);
  g_free (name);
  return path;
}

static gchar *
icon_copy_path (GogGame *game)
{
  gchar *name = g_strdup_printf ("%" G_GINT64_FORMAT ".png", gog_game_get_id (game));
  gchar *path = g_build_filename (g_get_user_data_dir (), "gog-manager", "icons", name, NULL);
  g_free (name);
  return path;
}

gboolean
gog_shortcut_exists (GogGame *game)
{
  gchar *path = gog_shortcut_path (game);
  gboolean exists = g_file_test (path, G_FILE_TEST_IS_REGULAR);
  g_free (path);
  return exists;
}

gchar *
gog_shortcut_exec_value (const char *path)
{
  /* Inside the quotes ", `, $ and \ take a backslash, and % is doubled so
   * it is no field code; then the value as a whole is a string, where every
   * backslash is doubled again. */
  GString *quoted = g_string_new ("\"");
  for (const char *p = path; *p; p++)
    {
      if (*p == '%')
        g_string_append_c (quoted, '%');
      else if (strchr ("\"`$\\", *p))
        g_string_append_c (quoted, '\\');
      g_string_append_c (quoted, *p);
    }
  g_string_append_c (quoted, '"');
  GString *out = g_string_new (NULL);
  for (const char *p = quoted->str; *p; p++)
    {
      if (*p == '\\')
        g_string_append_c (out, '\\');
      g_string_append_c (out, *p);
    }
  g_string_free (quoted, TRUE);
  return g_string_free (out, FALSE);
}

/* Name= and the like: one line, backslashes escaped. */
static gchar *
string_value (const char *text)
{
  GString *out = g_string_new (NULL);
  for (const char *p = text ? text : ""; *p; p++)
    {
      if (*p == '\\')
        g_string_append (out, "\\\\");
      else if (*p == '\n' || *p == '\r' || *p == '\t')
        g_string_append_c (out, ' ');
      else
        g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

/* Copies the library's icon next to the entry; the library may be on a disk
 * that isn't connected when the game is started. Returns the Icon= value. */
static gchar *
copy_icon (GogGame *game)
{
  const char *src = gog_game_get_icon_path (game);
  if (!src || !g_file_test (src, G_FILE_TEST_IS_REGULAR))
    return g_strdup ("applications-games");
  gchar *dest = icon_copy_path (game);
  gchar *dir = g_path_get_dirname (dest);
  g_mkdir_with_parents (dir, 0755);
  g_free (dir);
  GFile *from = g_file_new_for_path (src);
  GFile *to = g_file_new_for_path (dest);
  GError *error = NULL;
  gboolean ok = g_file_copy (from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_object_unref (from);
  g_object_unref (to);
  if (!ok)
    {
      g_warning ("Could not copy the icon for %s: %s", gog_game_get_title (game), error->message);
      g_clear_error (&error);
      g_free (dest);
      return g_strdup ("applications-games");
    }
  return dest;
}

gboolean
gog_shortcut_create (GogGame *game, GError **error)
{
  const char *install_dir = gog_game_get_install_path (game);
  if (!gog_game_get_installed (game) || !install_dir || !*install_dir)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is not installed",
                   gog_game_get_title (game));
      return FALSE;
    }

  /* A play.sh of the user's own is fine to start; gog-manager just doesn't
   * rewrite it. */
  GError *script_error = NULL;
  if (!gog_write_play_script (game, &script_error) &&
      !g_error_matches (script_error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    {
      g_propagate_error (error, script_error);
      return FALSE;
    }
  g_clear_error (&script_error);

  gchar *script = g_build_filename (install_dir, "play.sh", NULL);
  gchar *exec = gog_shortcut_exec_value (script);
  gchar *name = string_value (gog_game_get_title (game));
  gchar *icon = copy_icon (game);
  gchar *icon_value = string_value (icon);
  gchar *contents = g_strdup_printf ("[Desktop Entry]\n"
                                     "Type=Application\n"
                                     "Name=%s\n"
                                     "Comment=Installed with gog-manager\n"
                                     "Exec=%s\n"
                                     "Icon=%s\n"
                                     "Terminal=false\n"
                                     "Categories=Game;\n"
                                     "X-GOG-Manager-Game=%" G_GINT64_FORMAT "\n",
                                     name, exec, icon_value, gog_game_get_id (game));

  gchar *path = gog_shortcut_path (game);
  gchar *dir = g_path_get_dirname (path);
  gboolean ok = g_mkdir_with_parents (dir, 0755) == 0;
  if (!ok)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not create %s: %s", dir, g_strerror (saved_errno));
    }
  else
    ok = g_file_set_contents (path, contents, -1, error);

  g_free (dir);
  g_free (path);
  g_free (contents);
  g_free (icon_value);
  g_free (icon);
  g_free (name);
  g_free (exec);
  g_free (script);
  return ok;
}

static gboolean
remove_file (const char *path, GError **error)
{
  if (g_remove (path) == 0 || errno == ENOENT)
    return TRUE;
  int saved_errno = errno;
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
               "Could not remove %s: %s", path, g_strerror (saved_errno));
  return FALSE;
}

gboolean
gog_shortcut_remove (GogGame *game, GError **error)
{
  gchar *path = gog_shortcut_path (game);
  gchar *icon = icon_copy_path (game);
  gboolean ok = remove_file (path, error) && remove_file (icon, error);
  g_free (icon);
  g_free (path);
  return ok;
}
