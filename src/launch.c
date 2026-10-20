/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "launch.h"
#include "json_util.h"
#include "runner.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
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

static PlayTask *find_primary_task (const char *info_path);

/* Finds the primary play task in the install directory's goggame-*.info
 * manifests. Once DLC is installed into a game's directory there are several
 * of them (DLC manifests usually have no play tasks), so prefer the one named
 * after preferred_id, then fall back to the first manifest that has one. */
static PlayTask *
find_install_task (const char *install_dir, gint64 preferred_id)
{
  if (preferred_id != 0)
    {
      gchar *name = g_strdup_printf ("goggame-%" G_GINT64_FORMAT ".info", preferred_id);
      gchar *path = g_build_filename (install_dir, name, NULL);
      PlayTask *task = g_file_test (path, G_FILE_TEST_IS_REGULAR) ? find_primary_task (path) : NULL;
      g_free (path);
      g_free (name);
      if (task)
        return task;
    }

  GDir *d = g_dir_open (install_dir, 0, NULL);
  if (!d)
    return NULL;
  const char *name;
  PlayTask *task = NULL;
  while (!task && (name = g_dir_read_name (d)))
    {
      if (g_str_has_prefix (name, "goggame-") && g_str_has_suffix (name, ".info"))
        {
          gchar *path = g_build_filename (install_dir, name, NULL);
          task = find_primary_task (path);
          g_free (path);
        }
    }
  g_dir_close (d);
  return task;
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

typedef struct
{
  GogRunnerInfo *runner;
  gchar *install_dir;
  GogCommand *cmd;
} PrefixLaunch;

static GogLaunchFailedFunc failed_func;
static gpointer failed_data;

void
gog_launch_set_failed_func (GogLaunchFailedFunc func, gpointer user_data)
{
  failed_func = func;
  failed_data = user_data;
}

void
gog_launch_clear_failed_func (gpointer user_data)
{
  if (failed_data == user_data)
    gog_launch_set_failed_func (NULL, NULL);
}

/* On the main thread, where the handler is set and cleared. */
static gboolean
report_failed_idle (gpointer data)
{
  if (failed_func)
    failed_func (data, failed_data);
  g_free (data);
  return G_SOURCE_REMOVE;
}

static gpointer
prefix_launch_thread (gpointer data)
{
  PrefixLaunch *pl = data;
  GError *error = NULL;
  if (!runner_prepare_prefix (pl->runner, pl->install_dir, &error)
      || !spawn_detached (pl->cmd->argv, pl->cmd->envp, pl->install_dir, &error))
    {
      gchar *msg = g_strdup_printf ("Could not start the game: %s",
                                    error ? error->message : "unknown error");
      g_warning ("%s (%s)", msg, pl->install_dir);
      g_idle_add (report_failed_idle, msg); /* takes msg */
    }
  g_clear_error (&error);
  gog_command_free (pl->cmd);
  gog_runner_info_free (pl->runner);
  g_free (pl->install_dir);
  g_free (pl);
  return NULL;
}

/* Spawns a Wine/Proton command whose paths go through c:\game. A prefix
 * that is missing or was never set up (deleted, copied from elsewhere, an
 * install that failed half way) is rebuilt first, on a thread, because a
 * fresh Proton bootstrap takes many seconds. Takes ownership of cmd. */
static gboolean
spawn_in_prefix (GogRunnerInfo *runner, const char *install_dir, GogCommand *cmd, GError **error)
{
  if (runner_prefix_is_bootstrapped (runner->kind, install_dir))
    {
      gchar *wine_root = runner_wine_root_dir (runner->kind, install_dir);
      gboolean ok = runner_link_c_game (wine_root, install_dir, error)
                    && spawn_detached (cmd->argv, cmd->envp, install_dir, error);
      g_free (wine_root);
      gog_command_free (cmd);
      return ok;
    }

  PrefixLaunch *pl = g_new0 (PrefixLaunch, 1);
  pl->runner = gog_runner_info_new (runner->kind, runner->name, runner->bin_path);
  pl->install_dir = g_strdup (install_dir);
  pl->cmd = cmd;
  g_thread_unref (g_thread_new ("gog-prefix-launch", prefix_launch_thread, pl));
  return TRUE;
}

/* A runner view of the game's recorded runner. Borrows the game's strings;
 * nothing to free. */
static GogRunnerInfo
runner_from_game (GogGame *game)
{
  GogRunnerInfo info;
  info.kind = gog_game_get_runner_kind (game);
  info.name = (gchar *) "";
  info.bin_path = (gchar *) gog_game_get_runner_path (game);
  return info;
}

static gboolean
check_runner (GogGame *game, GError **error)
{
  const char *path = gog_game_get_runner_path (game);
  if (!path)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Wine/Proton runner is recorded for %s; choose one under Runner on its page",
                   gog_game_get_title (game));
      return FALSE;
    }

  gboolean present;
  if (gog_game_get_runner_kind (game) == GOG_RUNNER_PROTON)
    {
      gchar *proton_bin = g_build_filename (path, "proton", NULL);
      present = g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR);
      g_free (proton_bin);
    }
  else
    present = g_file_test (path, G_FILE_TEST_IS_EXECUTABLE);
  if (!present)
    {
      gchar *name = g_path_get_basename (path);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "%s, the runner %s uses, is no longer on this computer; choose another under "
                   "Runner on its page",
                   name, gog_game_get_title (game));
      g_free (name);
      return FALSE;
    }
  return TRUE;
}

/* Manifest paths may use Windows separators even in Linux builds. */
static gchar *
unix_slashes (const char *path)
{
  gchar *copy = g_strdup (path ? path : "");
  for (gchar *p = copy; *p; p++)
    if (*p == '\\')
      *p = '/';
  return copy;
}

/* What a game is started with: its manifest's primary task, or (no
 * manifest) start.sh for a native game and the first non-uninstaller .exe
 * for Wine/Proton, as a task with no working directory or arguments. NULL
 * when there is nothing to run. */
static PlayTask *
resolve_play_task (GogGame *game, const char *install_dir)
{
  /* A DLC shares its base game's directory; launching it means launching
   * the base game. */
  GogGame *base = gog_game_get_base_game (game);
  gint64 manifest_id = base ? gog_game_get_id (base) : gog_game_get_id (game);
  PlayTask *task = find_install_task (install_dir, manifest_id);
  if (task)
    return task;

  /* No manifest -- fall back to convention. */
  gchar *path = NULL;
  if (gog_game_get_runner_kind (game) == GOG_RUNNER_NATIVE)
    {
      gchar *start_sh = g_build_filename (install_dir, "start.sh", NULL);
      if (g_file_test (start_sh, G_FILE_TEST_IS_REGULAR))
        path = g_strdup ("start.sh");
      g_free (start_sh);
    }
  else
    path = find_final_resort_exe (install_dir);
  if (!path)
    return NULL;

  task = g_new0 (PlayTask, 1);
  task->path = path;
  return task;
}

static void
set_nothing_to_launch_error (GogGame *game, GError **error)
{
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
               gog_game_get_runner_kind (game) == GOG_RUNNER_NATIVE
                 ? "Could not find a way to launch %s (no goggame-*.info or start.sh)"
                 : "Could not find a way to launch %s (no goggame-*.info or .exe)",
               gog_game_get_title (game));
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

  if (kind != GOG_RUNNER_NATIVE && !check_runner (game, error))
    return FALSE;

  PlayTask *task = resolve_play_task (game, install_dir);
  if (!task)
    {
      set_nothing_to_launch_error (game, error);
      return FALSE;
    }

  gboolean ok;
  if (kind == GOG_RUNNER_NATIVE)
    {
      /* Both path and workingDir are relative to the install root. */
      gchar *rel = unix_slashes (task->path);
      gchar *full = g_build_filename (install_dir, rel, NULL);
      g_free (rel);
      if (g_chmod (full, 0755) != 0)
        g_warning ("Failed to set execute permission on %s", full);
      gchar *wd = unix_slashes (task->working_dir);
      gchar *cwd = g_build_filename (install_dir, wd, NULL);
      g_free (wd);

      GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
      g_ptr_array_add (argv, g_strdup (full));
      if (task->arguments && *task->arguments)
        {
          gchar **split = NULL;
          GError *parse_error = NULL;
          if (g_shell_parse_argv (task->arguments, NULL, &split, &parse_error))
            {
              for (gchar **a = split; *a; a++)
                g_ptr_array_add (argv, g_strdup (*a));
              g_strfreev (split);
            }
          else
            {
              g_warning ("Failed to parse command arguments: %s", parse_error->message);
              g_error_free (parse_error);
            }
        }
      g_ptr_array_add (argv, NULL);
      ok = spawn_detached ((gchar **) argv->pdata, NULL, cwd, error);
      g_ptr_array_unref (argv);
      g_free (cwd);
      g_free (full);
    }
  else
    {
      GogRunnerInfo runner = runner_from_game (game);
      GogCommand *cmd = runner_build_launch_command (&runner, install_dir, task->path,
                                                       task->working_dir, task->arguments);
      ok = spawn_in_prefix (&runner, install_dir, cmd, error);
    }
  play_task_free (task);

  /* Keeps play.sh in step with the runner, and gives games installed
   * before it existed one. Never stands in the way of playing. */
  GError *script_error = NULL;
  if (ok && !gog_write_play_script (game, &script_error))
    {
      g_warning ("Could not write play.sh: %s", script_error->message);
      g_clear_error (&script_error);
    }
  return ok;
}

#define PLAY_SCRIPT_MARKER "# Written by gog-manager"

/* TRUE when path is missing or is a play.sh gog-manager wrote; a play.sh
 * the game shipped or the user wrote is left alone. */
static gboolean
play_script_is_ours (const char *path)
{
  if (!g_file_test (path, G_FILE_TEST_EXISTS) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    return TRUE;
  if (g_file_test (path, G_FILE_TEST_IS_SYMLINK) || !g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return FALSE;
  gchar *contents = NULL;
  gboolean ours = g_file_get_contents (path, &contents, NULL, NULL) &&
                  strstr (contents, "\n" PLAY_SCRIPT_MARKER) != NULL;
  g_free (contents);
  return ours;
}

gboolean
gog_write_play_script (GogGame *game, GError **error)
{
  const char *install_dir = gog_game_get_install_path (game);
  GogRunnerKind kind = gog_game_get_runner_kind (game);
  if (!gog_game_get_installed (game) || !install_dir || !*install_dir)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s is not installed",
                   gog_game_get_title (game));
      return FALSE;
    }
  if (kind != GOG_RUNNER_NATIVE && !gog_game_get_runner_path (game))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Wine/Proton runner is recorded for %s", gog_game_get_title (game));
      return FALSE;
    }

  gchar *script_path = g_build_filename (install_dir, "play.sh", NULL);
  if (!play_script_is_ours (script_path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                   "%s was not written by gog-manager; leaving it alone", script_path);
      g_free (script_path);
      return FALSE;
    }

  PlayTask *task = resolve_play_task (game, install_dir);
  if (!task)
    {
      set_nothing_to_launch_error (game, error);
      g_free (script_path);
      return FALSE;
    }

  /* Paths inside the game folder are written relative to the script, so it
   * keeps working if the folder is moved or seen from another mount. */
  GString *s = g_string_new ("#!/bin/sh\n");
  /* One comment line: a line break in the title would end it. */
  gchar *title = g_strdup (gog_game_get_title (game) ? gog_game_get_title (game) : "");
  g_strdelimit (title, "\r\n", ' ');
  g_string_append_printf (s, PLAY_SCRIPT_MARKER " to start %s without it.\n", title);
  g_free (title);
  g_string_append (s, "# It is rewritten when the game is played or its runner changes there.\n"
                      "# Delete the \"Written by gog-manager\" line to keep your own edits.\n"
                      "here=$(cd \"$(dirname \"$0\")\" && pwd) || exit 1\n");

  if (kind == GOG_RUNNER_NATIVE)
    {
      gchar *rel = unix_slashes (task->path);
      gchar *wd = unix_slashes (task->working_dir);
      gchar *q_rel = g_shell_quote (rel);
      g_string_append (s, "cd \"$here\"");
      if (*wd)
        {
          gchar *q_wd = g_shell_quote (wd);
          g_string_append_printf (s, "/%s", q_wd);
          g_free (q_wd);
        }
      g_string_append_printf (s, " || exit 1\nexec \"$here\"/%s", q_rel);
      /* Split the manifest's arguments as gog_launch_game() does and quote
       * each, so /bin/sh does no $, ; or glob expansion it would not. */
      gchar **split = NULL;
      if (task->arguments && *task->arguments
          && g_shell_parse_argv (task->arguments, NULL, &split, NULL))
        {
          for (gchar **a = split; *a; a++)
            {
              gchar *q = g_shell_quote (*a);
              g_string_append_printf (s, " %s", q);
              g_free (q);
            }
          g_strfreev (split);
        }
      g_string_append (s, " \"$@\"\n");
      g_free (q_rel);
      g_free (wd);
      g_free (rel);
    }
  else
    {
      GogRunnerInfo runner = runner_from_game (game);
      GogCommand *cmd = runner_build_launch_command (&runner, install_dir, task->path,
                                                       task->working_dir, task->arguments);
      if (kind == GOG_RUNNER_PROTON)
        {
          gchar *client = g_build_filename (g_get_user_data_dir (), "gog-manager",
                                             "steam-compat-client", NULL);
          gchar *q_client = g_shell_quote (client);
          g_string_append_printf (s,
              "root=\"$here/proton-prefix/pfx\"\n"
              "export STEAM_COMPAT_DATA_PATH=\"$here/proton-prefix\"\n"
              "export STEAM_COMPAT_CLIENT_INSTALL_PATH=%s\n"
              "mkdir -p \"$STEAM_COMPAT_CLIENT_INSTALL_PATH\"\n",
              q_client);
          g_free (q_client);
          g_free (client);
        }
      else
        g_string_append (s,
            "root=\"$here/prefix\"\n"
            "export WINEPREFIX=\"$root\"\n"
            "export WINEDLLOVERRIDES=\"winemenubuilder.exe=d\"\n");
      /* C:\game is a link gog-manager makes in the prefix; without it the
       * game's paths lead nowhere. */
      g_string_append (s,
          "if [ ! -e \"$root/dosdevices/c:/game\" ]; then\n"
          "  echo \"The Wine/Proton prefix is not set up; start the game once from gog-manager.\" >&2\n"
          "  exit 1\n"
          "fi\n"
          "cd \"$here\" || exit 1\n"
          "exec");
      for (gchar **a = cmd->argv; *a; a++)
        {
          gchar *q = g_shell_quote (*a);
          g_string_append_printf (s, " %s", q);
          g_free (q);
        }
      g_string_append (s, " \"$@\"\n");
      gog_command_free (cmd);
    }
  play_task_free (task);

  gboolean ok = g_file_set_contents (script_path, s->str, s->len, error);
  if (ok && g_chmod (script_path, 0755) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not make %s executable: %s", script_path, g_strerror (saved_errno));
      ok = FALSE;
    }
  g_string_free (s, TRUE);
  g_free (script_path);
  return ok;
}

gboolean
gog_open_winecfg (GogGame *game, GError **error)
{
  if (!gog_game_get_installed (game) || gog_game_get_runner_kind (game) != GOG_RUNNER_WINE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "winecfg is only available for games installed with Wine");
      return FALSE;
    }
  if (!check_runner (game, error))
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
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "regedit is only available for games installed with Wine or Proton");
      return FALSE;
    }
  if (!check_runner (game, error))
    return FALSE;
  GogRunnerInfo runner = runner_from_game (game);
  GogCommand *cmd = runner_build_regedit_command (&runner, gog_game_get_install_path (game));
  gboolean ok = spawn_detached (cmd->argv, cmd->envp, gog_game_get_install_path (game), error);
  gog_command_free (cmd);
  return ok;
}
