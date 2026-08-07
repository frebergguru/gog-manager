/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "launch.h"
#include "json_util.h"
#include "runner.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct
{
  gchar *name;
  gchar *path;         /* relative, Windows-style backslashes for FileTasks */
  gchar *working_dir;  /* relative, may be NULL */
  gchar *arguments;    /* may be NULL */
  gboolean is_primary;
  gchar *category;
  gchar *type;          /* "FileTask" or "URLTask" */
} PlayTask;

static void
play_task_free (PlayTask *t)
{
  if (!t)
    return;
  g_free (t->name);
  g_free (t->path);
  g_free (t->working_dir);
  g_free (t->arguments);
  g_free (t->category);
  g_free (t->type);
  g_free (t);
}

static gchar *
find_info_file (const char *install_dir)
{
  GDir *d = g_dir_open (install_dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  gchar *found = NULL;
  while ((name = g_dir_read_name (d)))
    {
      if (g_str_has_prefix (name, "goggame-") && g_str_has_suffix (name, ".info"))
        {
          found = g_build_filename (install_dir, name, NULL);
          break;
        }
    }
  g_dir_close (d);
  return found;
}

static PlayTask *
find_primary_task (const char *info_path)
{
  GError *error = NULL;
  json_object *root = ju_load_file (info_path, &error);
  if (!root)
    {
      g_clear_error (&error);
      return NULL;
    }

  json_object *tasks = ju_get_array (root, "playTasks");
  PlayTask *best = NULL;
  PlayTask *first_primary = NULL;

  if (tasks)
    {
      size_t n = json_object_array_length (tasks);
      for (size_t i = 0; i < n; i++)
        {
          json_object *item = json_object_array_get_idx (tasks, i);
          if (!ju_get_bool (item, "isPrimary", FALSE))
            continue;
          const char *type = ju_get_string (item, "type", "");
          if (g_strcmp0 (type, "FileTask") != 0)
            continue;

          PlayTask *t = g_new0 (PlayTask, 1);
          t->name = g_strdup (ju_get_string (item, "name", ""));
          const char *path_raw = ju_get_string (item, "path", "");
          const char *workdir_raw = ju_get_string (item, "workingDir", NULL);

          if (path_raw[0] == '\0' || g_str_has_prefix (path_raw, "/") || g_strstr_len (path_raw, -1, "..") ||
              (workdir_raw && g_str_has_prefix (workdir_raw, "/")) || (workdir_raw && g_strstr_len (workdir_raw, -1, "..")))
            {
              g_warning ("Rejected manifest task with suspicious path: %s workdir: %s", path_raw, workdir_raw ? workdir_raw : "");
              play_task_free (t);
              continue;
            }

          t->path = g_strdup (path_raw);
          t->working_dir = g_strdup (workdir_raw);
          t->arguments = g_strdup (ju_get_string (item, "arguments", NULL));
          t->is_primary = TRUE;
          t->category = g_strdup (ju_get_string (item, "category", ""));
          t->type = g_strdup (type);

          if (g_strcmp0 (t->category, "launcher") == 0)
            {
              best = t;
              break;
            }
          if (!first_primary)
            first_primary = t;
          else
            play_task_free (t);
        }
    }

  json_object_put (root);

  if (best)
    {
      if (first_primary && first_primary != best)
        play_task_free (first_primary);
      return best;
    }
  return first_primary;
}

static gchar *
find_final_resort_exe (const char *install_dir)
{
  GDir *d = g_dir_open (install_dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  gchar *found = NULL;
  while ((name = g_dir_read_name (d)))
    {
      gchar *lower = g_ascii_strdown (name, -1);
      gboolean is_exe = g_str_has_suffix (lower, ".exe");
      gboolean is_uninstaller = g_str_has_prefix (lower, "unins");
      g_free (lower);
      if (is_exe && !is_uninstaller)
        {
          found = g_strdup (name);
          break;
        }
    }
  g_dir_close (d);
  return found;
}

static gboolean
spawn_detached (gchar **argv, gchar **envp, const char *cwd, GError **error)
{
  return g_spawn_async (cwd, argv, envp,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, error);
}

static GogRunnerInfo
runner_from_game (GogGame *game)
{
  GogRunnerInfo info;
  info.kind = gog_game_get_runner_kind (game);
  info.name = g_strdup ("");
  info.bin_path = g_strdup (gog_game_get_runner_path (game));
  return info;
}

gboolean
gog_launch_game (GogGame *game, GError **error)
{
  if (!gog_game_get_installed (game) || !gog_game_get_install_path (game))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is not installed",
                   gog_game_get_title (game));
      return FALSE;
    }

  const char *install_dir = gog_game_get_install_path (game);
  GogRunnerKind kind = gog_game_get_runner_kind (game);

  gchar *info_path = find_info_file (install_dir);
  PlayTask *task = info_path ? find_primary_task (info_path) : NULL;
  g_free (info_path);

  if (task)
    {
      gboolean ok;
      if (kind == GOG_RUNNER_NATIVE)
        {
          gchar *full = g_build_filename (install_dir, task->working_dir ? task->working_dir : "",
                                           task->path, NULL);
          if (g_chmod (full, 0755) != 0)
            g_warning ("Failed to set execute permission on %s", full);
          gchar *argv[] = { full, NULL };
          gchar *cwd = g_build_filename (install_dir, task->working_dir ? task->working_dir : "", NULL);
          ok = spawn_detached (argv, NULL, cwd, error);
          g_free (cwd);
          g_free (full);
        }
      else
        {
          GogRunnerInfo runner = runner_from_game (game);
          GogCommand *cmd = runner_build_launch_command (&runner, install_dir, task->path,
                                                           task->working_dir, task->arguments);
          ok = spawn_detached (cmd->argv, cmd->envp, install_dir, error);
          gog_command_free (cmd);
        }
      play_task_free (task);
      return ok;
    }

  /* No manifest -- fall back to convention. */
  if (kind == GOG_RUNNER_NATIVE)
    {
      gchar *start_sh = g_build_filename (install_dir, "start.sh", NULL);
      if (g_file_test (start_sh, G_FILE_TEST_IS_REGULAR))
        {
          if (g_chmod (start_sh, 0755) != 0)
            g_warning ("Failed to set execute permission on %s", start_sh);
          gchar *argv[] = { start_sh, NULL };
          gboolean ok = spawn_detached (argv, NULL, install_dir, error);
          g_free (start_sh);
          return ok;
        }
      g_free (start_sh);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Could not find a way to launch %s (no goggame-*.info or start.sh)",
                   gog_game_get_title (game));
      return FALSE;
    }

  gchar *exe = find_final_resort_exe (install_dir);
  if (!exe)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Could not find a way to launch %s (no goggame-*.info or .exe)",
                   gog_game_get_title (game));
      return FALSE;
    }

  GogRunnerInfo runner = runner_from_game (game);
  GogCommand *cmd = runner_build_launch_command (&runner, install_dir, exe, NULL, NULL);
  gboolean ok = spawn_detached (cmd->argv, cmd->envp, install_dir, error);
  gog_command_free (cmd);
  g_free (exe);
  return ok;
}

gboolean
gog_open_winecfg (GogGame *game, GError **error)
{
  if (!gog_game_get_installed (game) || gog_game_get_runner_kind (game) != GOG_RUNNER_WINE)
    return FALSE;
  GogRunnerInfo runner = runner_from_game (game);
  GogCommand *cmd = runner_build_winecfg_command (&runner, gog_game_get_install_path (game));
  gboolean ok = spawn_detached (cmd->argv, cmd->envp, gog_game_get_install_path (game), error);
  gog_command_free (cmd);
  return ok;
}

gboolean
gog_open_regedit (GogGame *game, GError **error)
{
  if (!gog_game_get_installed (game) || gog_game_get_runner_kind (game) == GOG_RUNNER_NONE ||
      gog_game_get_runner_kind (game) == GOG_RUNNER_NATIVE)
    return FALSE;
  GogRunnerInfo runner = runner_from_game (game);
  GogCommand *cmd = runner_build_regedit_command (&runner, gog_game_get_install_path (game));
  gboolean ok = spawn_detached (cmd->argv, cmd->envp, gog_game_get_install_path (game), error);
  gog_command_free (cmd);
  return ok;
}
