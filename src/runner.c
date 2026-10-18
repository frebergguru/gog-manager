/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "runner.h"
#include "proton_fetch.h"
#include <glib/gstdio.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

GogRunnerInfo *
gog_runner_info_new (GogRunnerKind kind, const char *name, const char *bin_path)
{
  GogRunnerInfo *info = g_new0 (GogRunnerInfo, 1);
  info->kind = kind;
  info->name = g_strdup (name);
  info->bin_path = g_strdup (bin_path);
  return info;
}

void
gog_runner_info_free (GogRunnerInfo *info)
{
  if (!info)
    return;
  g_free (info->name);
  g_free (info->bin_path);
  g_free (info);
}

void
gog_command_free (GogCommand *cmd)
{
  if (!cmd)
    return;
  g_strfreev (cmd->argv);
  g_strfreev (cmd->envp);
  g_free (cmd);
}

/* Add every Proton build found directly under parent_dir (each is a
 * subdirectory containing a "proton" executable) to out. */
static void
scan_proton_dir (const char *parent_dir, GPtrArray *out)
{
  GDir *d = g_dir_open (parent_dir, 0, NULL);
  if (!d)
    return;

  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      gchar *sub = g_build_filename (parent_dir, name, NULL);
      gchar *proton_bin = g_build_filename (sub, "proton", NULL);
      if (g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR))
        g_ptr_array_add (out, gog_runner_info_new (GOG_RUNNER_PROTON, name, sub));
      g_free (proton_bin);
      g_free (sub);
    }
  g_dir_close (d);
}

GPtrArray *
runner_discover_all (GogConfig *config)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_runner_info_free);

  gchar *wine_path = g_find_program_in_path ("wine");
  if (wine_path)
    {
      g_ptr_array_add (out, gog_runner_info_new (GOG_RUNNER_WINE, "System Wine", wine_path));
      g_free (wine_path);
    }

  gchar *own_proton_dir = proton_fetch_get_tools_dir ();
  scan_proton_dir (own_proton_dir, out);
  g_free (own_proton_dir);

  const char *home = g_get_home_dir ();

  gchar *heroic_proton = g_build_filename (home, ".config", "heroic", "tools", "proton", NULL);
  scan_proton_dir (heroic_proton, out);
  g_free (heroic_proton);

  gchar *steam_compat1 = g_build_filename (home, ".steam", "steam", "compatibilitytools.d", NULL);
  scan_proton_dir (steam_compat1, out);
  g_free (steam_compat1);

  gchar *steam_compat2 = g_build_filename (home, ".local", "share", "Steam",
                                            "compatibilitytools.d", NULL);
  scan_proton_dir (steam_compat2, out);
  g_free (steam_compat2);

  scan_proton_dir ("/usr/share/steam/compatibilitytools.d", out);

  if (config && config->custom_runner_paths)
    {
      for (GStrv p = config->custom_runner_paths; *p; p++)
        {
          /* Encoded as "wine=<path>" or "proton=<path>" by the preferences UI. */
          if (g_str_has_prefix (*p, "wine="))
            {
              const char *path = *p + 5;
              if (g_path_is_absolute (path) && g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
                g_ptr_array_add (out, gog_runner_info_new (GOG_RUNNER_WINE, path, path));
            }
          else if (g_str_has_prefix (*p, "proton="))
            {
              const char *path = *p + 7;
              if (g_path_is_absolute (path) && g_file_test (path, G_FILE_TEST_IS_DIR))
                {
                  gchar *dir = g_strdup (path);
                  gchar *base = g_path_get_basename (dir);
                  g_ptr_array_add (out, gog_runner_info_new (GOG_RUNNER_PROTON, base, dir));
                  g_free (base);
                  g_free (dir);
                }
            }
        }
    }

  return out;
}

gchar *
runner_prefix_dir_for_install (GogRunnerKind kind, const char *install_dir)
{
  return g_build_filename (install_dir, kind == GOG_RUNNER_PROTON ? "proton-prefix" : "prefix", NULL);
}

gchar *
runner_wine_root_dir (GogRunnerKind kind, const char *install_dir)
{
  gchar *prefix = runner_prefix_dir_for_install (kind, install_dir);
  if (kind == GOG_RUNNER_PROTON)
    {
      gchar *pfx = g_build_filename (prefix, "pfx", NULL);
      g_free (prefix);
      return pfx;
    }
  return prefix;
}

gboolean
runner_link_c_game (const char *wine_root, const char *install_dir, GError **error)
{
  gchar *dosdevices = g_build_filename (wine_root, "dosdevices", NULL);
  if (g_mkdir_with_parents (dosdevices, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create %s", dosdevices);
      g_free (dosdevices);
      return FALSE;
    }

  gchar *link_dir = g_build_filename (dosdevices, "c:", NULL);
  g_free (dosdevices);
  gchar *game_link = g_build_filename (link_dir, "game", NULL);

  /* Wine creates dosdevices/c: as a symlink to ../drive_c, and the kernel
   * resolves a relative symlink target against the directory the link
   * physically lives in -- so compute the "../" chain from the *resolved*
   * c: directory (and resolved install_dir), not from the lexical path,
   * which is one level deeper and would point above install_dir. */
  gchar *real_link_dir = realpath (link_dir, NULL);
  gchar *real_install = realpath (install_dir, NULL);
  gchar *relative = NULL;
  if (real_link_dir && real_install)
    {
      GFile *install_gfile = g_file_new_for_path (real_install);
      GFile *link_gfile = g_file_new_for_path (real_link_dir);
      gchar *down = g_file_get_relative_path (install_gfile, link_gfile);
      if (down)
        {
          GString *up = g_string_new (NULL);
          gchar **segments = g_strsplit (down, G_DIR_SEPARATOR_S, -1);
          for (guint i = 0; segments[i]; i++)
            g_string_append (up, ".." G_DIR_SEPARATOR_S);
          relative = g_string_free (up, FALSE);
          g_strfreev (segments);
          g_free (down);
        }
      g_object_unref (install_gfile);
      g_object_unref (link_gfile);
    }
  /* Should not happen given how wine_root is always built under install_dir,
   * but fall back to an absolute target if it ever does. */
  if (!relative)
    relative = g_strdup (real_install ? real_install : install_dir);
  free (real_link_dir);
  free (real_install);
  g_free (link_dir);

  if (g_file_test (game_link, G_FILE_TEST_IS_SYMLINK))
    {
      gchar *target = g_file_read_link (game_link, NULL);
      gboolean same = target && g_strcmp0 (target, relative) == 0;
      g_free (target);
      if (same)
        {
          g_free (relative);
          g_free (game_link);
          return TRUE;
        }
      /* Stale or wrong link (e.g. one created by an older version that
       * pointed one directory too high) -- replace it. */
      g_remove (game_link);
    }
  else if (g_file_test (game_link, G_FILE_TEST_EXISTS))
    {
      /* A real file/dir is in the way; only an empty directory can be
       * replaced safely. If this fails, symlink() below reports it. */
      g_rmdir (game_link);
    }

  gboolean ok = (symlink (relative, game_link) == 0);
  if (!ok)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not symlink %s -> %s: %s", game_link, relative,
                   g_strerror (saved_errno));
    }

  g_free (relative);
  g_free (game_link);
  return ok;
}

static gchar *
win_backslash (const char *relpath)
{
  gchar *copy = g_strdup (relpath ? relpath : "");
  for (gchar *p = copy; *p; p++)
    if (*p == '/')
      *p = '\\';
  return copy;
}

/* Splits a manifest's arguments the way a Windows program splits its own
 * command line (CommandLineToArgvW): backslashes are literal unless they
 * precede a quote, so "-path C:\Data" keeps its backslash, which POSIX
 * shell parsing would drop. Each argument is appended to argv. */
static void
append_windows_args (GPtrArray *argv, const char *args)
{
  const char *p = args;
  while (*p)
    {
      while (*p == ' ' || *p == '\t')
        p++;
      if (!*p)
        break;

      GString *arg = g_string_new (NULL);
      gboolean quoted = FALSE;
      while (*p && (quoted || (*p != ' ' && *p != '\t')))
        {
          if (*p == '\\')
            {
              gsize n = 0;
              while (*p == '\\')
                {
                  n++;
                  p++;
                }
              if (*p == '"')
                {
                  /* 2n backslashes + quote: n backslashes, quote toggles;
                   * 2n+1: n backslashes and a literal quote. */
                  for (gsize i = 0; i < n / 2; i++)
                    g_string_append_c (arg, '\\');
                  if (n % 2)
                    {
                      g_string_append_c (arg, '"');
                      p++;
                    }
                }
              else
                for (gsize i = 0; i < n; i++)
                  g_string_append_c (arg, '\\');
            }
          else if (*p == '"')
            {
              /* "" inside quotes is a literal quote. */
              if (quoted && p[1] == '"')
                {
                  g_string_append_c (arg, '"');
                  p += 2;
                }
              else
                {
                  quoted = !quoted;
                  p++;
                }
            }
          else
            g_string_append_c (arg, *p++);
        }
      g_ptr_array_add (argv, g_string_free (arg, FALSE));
    }
}

/* Builds the base argv (before program-specific verbs) and the base envp
 * shared by install/launch/tool commands for a given runner + install dir. */
static void
build_base (GogRunnerInfo *runner, const char *install_dir, gchar ***envp_out, gchar **wine_root_out)
{
  gchar *prefix = runner_prefix_dir_for_install (runner->kind, install_dir);
  gchar **envp = g_get_environ ();

  if (runner->kind == GOG_RUNNER_PROTON)
    {
      gchar *dummy_client = g_build_filename (g_get_user_data_dir (), "gog-manager",
                                               "steam-compat-client", NULL);
      g_mkdir_with_parents (dummy_client, 0755);
      envp = g_environ_setenv (envp, "STEAM_COMPAT_DATA_PATH", prefix, TRUE);
      envp = g_environ_setenv (envp, "STEAM_COMPAT_CLIENT_INSTALL_PATH", dummy_client, TRUE);
      *wine_root_out = g_build_filename (prefix, "pfx", NULL);
      g_free (dummy_client);
    }
  else
    {
      envp = g_environ_setenv (envp, "WINEPREFIX", prefix, TRUE);
      envp = g_environ_setenv (envp, "WINEDLLOVERRIDES", "winemenubuilder.exe=d", TRUE);
      *wine_root_out = g_strdup (prefix);
    }

  g_free (prefix);
  *envp_out = envp;
}

GogCommand *
runner_build_bootstrap_command (GogRunnerInfo *runner, const char *install_dir)
{
  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  g_free (wine_root);

  GPtrArray *argv = g_ptr_array_new ();
  if (runner->kind == GOG_RUNNER_PROTON)
    {
      g_ptr_array_add (argv, g_build_filename (runner->bin_path, "proton", NULL));
      g_ptr_array_add (argv, g_strdup ("run"));
      g_ptr_array_add (argv, g_strdup ("wineboot"));
    }
  else
    {
      g_ptr_array_add (argv, g_strdup (runner->bin_path));
      g_ptr_array_add (argv, g_strdup ("wineboot"));
    }
  g_ptr_array_add (argv, g_strdup ("-u"));
  g_ptr_array_add (argv, NULL);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
  cmd->envp = envp;
  return cmd;
}

gboolean
runner_ensure_prefix_dir (GogRunnerKind kind, const char *install_dir, GError **error)
{
  gchar *prefix = runner_prefix_dir_for_install (kind, install_dir);
  gboolean ok = (g_mkdir_with_parents (prefix, 0755) == 0);
  if (!ok)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not create prefix directory %s: %s", prefix, g_strerror (saved_errno));
    }
  g_free (prefix);
  return ok;
}

gboolean
runner_prefix_is_bootstrapped (GogRunnerKind kind, const char *install_dir)
{
  gchar *wine_root = runner_wine_root_dir (kind, install_dir);
  gchar *c_drive = g_build_filename (wine_root, "dosdevices", "c:", NULL);
  gboolean ok = g_file_test (c_drive, G_FILE_TEST_IS_DIR);
  g_free (c_drive);
  g_free (wine_root);
  return ok;
}

gboolean
runner_prepare_prefix (GogRunnerInfo *runner, const char *install_dir, GError **error)
{
  if (!runner_ensure_prefix_dir (runner->kind, install_dir, error))
    return FALSE;

  if (!runner_prefix_is_bootstrapped (runner->kind, install_dir))
    {
      GogCommand *boot = runner_build_bootstrap_command (runner, install_dir);
      gint status = 0;
      GError *spawn_err = NULL;
      if (!g_spawn_sync (install_dir, boot->argv, boot->envp,
                         G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, &status, &spawn_err))
        {
          g_propagate_error (error, spawn_err);
          gog_command_free (boot);
          return FALSE;
        }
      gog_command_free (boot);
      /* A non-zero exit is not fatal by itself (wineboot often complains);
       * what matters is whether the skeleton now exists. */
      if (!runner_prefix_is_bootstrapped (runner->kind, install_dir))
        {
          gchar *prefix = runner_prefix_dir_for_install (runner->kind, install_dir);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Wine/Proton could not set up the prefix in %s", prefix);
          g_free (prefix);
          return FALSE;
        }
    }

  gchar *wine_root = runner_wine_root_dir (runner->kind, install_dir);
  gboolean ok = runner_link_c_game (wine_root, install_dir, error);
  g_free (wine_root);
  return ok;
}

GogCommand *
runner_build_install_command (GogRunnerInfo *runner, const char *installer_path,
                               const char *install_dir, const char *lang_code, gboolean silent)
{
  if (!runner || !runner->bin_path)
    {
      GogCommand *cmd = g_new0 (GogCommand, 1);
      return cmd;
    }

  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  g_free (wine_root);

  GPtrArray *argv = g_ptr_array_new ();

  if (runner->kind == GOG_RUNNER_PROTON)
    {
      gchar *proton_bin = g_build_filename (runner->bin_path, "proton", NULL);
      g_ptr_array_add (argv, proton_bin);
      g_ptr_array_add (argv, g_strdup ("run"));
    }
  else
    {
      g_ptr_array_add (argv, g_strdup (runner->bin_path));
    }

  g_ptr_array_add (argv, g_strdup (installer_path));
  g_ptr_array_add (argv, g_strdup ("/DIR=c:\\game"));
  g_ptr_array_add (argv, g_strdup_printf ("/LANG=%s", lang_code && *lang_code ? lang_code : "en"));
  g_ptr_array_add (argv, g_strdup ("/LOG=c:\\install.log"));

  if (silent)
    {
      g_ptr_array_add (argv, g_strdup ("/SAVEINF=c:\\setup.inf"));
      g_ptr_array_add (argv, g_strdup ("/SP-"));
      g_ptr_array_add (argv, g_strdup ("/SILENT"));
      g_ptr_array_add (argv, g_strdup ("/NORESTART"));
      g_ptr_array_add (argv, g_strdup ("/SUPPRESSMSGBOXES"));
    }

  g_ptr_array_add (argv, NULL);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
  cmd->envp = envp;
  return cmd;
}

GogCommand *
runner_build_launch_command (GogRunnerInfo *runner, const char *install_dir, const char *exe_relpath,
                              const char *working_dir_relpath, const char *arguments)
{
  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  g_free (wine_root);

  GPtrArray *argv = g_ptr_array_new ();

  if (runner->kind == GOG_RUNNER_PROTON)
    {
      g_ptr_array_add (argv, g_build_filename (runner->bin_path, "proton", NULL));
      g_ptr_array_add (argv, g_strdup ("run"));
    }
  else
    {
      g_ptr_array_add (argv, g_strdup (runner->bin_path));
    }

  gchar *win_exe = win_backslash (exe_relpath);
  gchar *win_wd = win_backslash (working_dir_relpath && *working_dir_relpath ? working_dir_relpath : ".");
  gchar *wd_arg = g_strdup_printf ("c:\\game\\%s", win_wd);
  gchar *exe_arg = g_strdup_printf ("c:\\game\\%s", win_exe);

  g_ptr_array_add (argv, g_strdup ("start"));
  g_ptr_array_add (argv, g_strdup ("/b"));
  g_ptr_array_add (argv, g_strdup ("/wait"));
  g_ptr_array_add (argv, g_strdup ("/d"));
  g_ptr_array_add (argv, wd_arg);
  g_ptr_array_add (argv, exe_arg);

  if (arguments && *arguments)
    append_windows_args (argv, arguments);

  g_free (win_exe);
  g_free (win_wd);
  g_ptr_array_add (argv, NULL);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
  cmd->envp = envp;
  return cmd;
}

GogCommand *
runner_build_winecfg_command (GogRunnerInfo *runner, const char *install_dir)
{
  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  g_free (wine_root);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  if (runner->kind == GOG_RUNNER_PROTON)
    {
      cmd->argv = g_new0 (gchar *, 1);
    }
  else
    {
      GPtrArray *argv = g_ptr_array_new ();
      g_ptr_array_add (argv, g_strdup (runner->bin_path));
      g_ptr_array_add (argv, g_strdup ("winecfg"));
      g_ptr_array_add (argv, NULL);
      cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
    }
  cmd->envp = envp;
  return cmd;
}

GogCommand *
runner_build_regedit_command (GogRunnerInfo *runner, const char *install_dir)
{
  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  g_free (wine_root);

  GPtrArray *argv = g_ptr_array_new ();
  if (runner->kind == GOG_RUNNER_PROTON)
    {
      g_ptr_array_add (argv, g_build_filename (runner->bin_path, "proton", NULL));
      g_ptr_array_add (argv, g_strdup ("run"));
    }
  else
    g_ptr_array_add (argv, g_strdup (runner->bin_path));
  g_ptr_array_add (argv, g_strdup ("regedit"));
  g_ptr_array_add (argv, NULL);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
  cmd->envp = envp;
  return cmd;
}

GogCommand *
runner_build_kill_command (GogRunnerInfo *runner, const char *install_dir)
{
  gchar **envp = NULL;
  gchar *wine_root = NULL;
  build_base (runner, install_dir, &envp, &wine_root);
  /* Proton only gets STEAM_COMPAT_DATA_PATH; wineserver finds its server
   * through WINEPREFIX. */
  envp = g_environ_setenv (envp, "WINEPREFIX", wine_root, TRUE);
  g_free (wine_root);

  gchar *wineserver = NULL;
  if (runner->kind == GOG_RUNNER_PROTON)
    {
      const char *layouts[] = { "files", "dist" };
      for (guint i = 0; i < G_N_ELEMENTS (layouts) && !wineserver; i++)
        {
          gchar *candidate = g_build_filename (runner->bin_path, layouts[i], "bin", "wineserver", NULL);
          if (g_file_test (candidate, G_FILE_TEST_IS_EXECUTABLE))
            wineserver = candidate;
          else
            g_free (candidate);
        }
      if (!wineserver)
        wineserver = g_build_filename (runner->bin_path, "files", "bin", "wineserver", NULL);
    }
  else
    {
      gchar *dir = g_path_get_dirname (runner->bin_path);
      wineserver = g_build_filename (dir, "wineserver", NULL);
      g_free (dir);
      if (!g_file_test (wineserver, G_FILE_TEST_IS_EXECUTABLE))
        {
          g_free (wineserver);
          wineserver = g_strdup ("wineserver");
        }
    }

  GPtrArray *argv = g_ptr_array_new ();
  g_ptr_array_add (argv, wineserver);
  g_ptr_array_add (argv, g_strdup ("-k"));
  g_ptr_array_add (argv, NULL);

  GogCommand *cmd = g_new0 (GogCommand, 1);
  cmd->argv = (gchar **) g_ptr_array_free (argv, FALSE);
  cmd->envp = envp;
  return cmd;
}

gboolean
runner_is_freetype_warning (const char *line)
{
  static const char *const parts[] = {
    "Wine cannot find the FreeType font library",
    "use TrueType fonts please install a version of FreeType",
    "or equal to 2.0.5.",
    "http://www.freetype.org",
  };
  if (!line)
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (parts); i++)
    if (g_str_has_prefix (line, parts[i]))
      return TRUE;
  return FALSE;
}

/* Which word sizes of libfreetype.so.6 are installed, read from the ELF
 * class byte: directory names alone can't tell (Fedora's /usr/lib is
 * 32-bit, Arch's is 64-bit). */
static void
find_freetype (gboolean *has32, gboolean *has64)
{
  static const char *const dirs[] = {
    "/usr/lib", "/usr/lib32", "/usr/lib64", "/usr/lib/i386-linux-gnu",
    "/usr/lib/x86_64-linux-gnu", "/lib", "/lib32", "/lib64", "/lib/i386-linux-gnu",
    "/lib/x86_64-linux-gnu",
  };
  *has32 = *has64 = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
      gchar *path = g_build_filename (dirs[i], "libfreetype.so.6", NULL);
      FILE *f = g_fopen (path, "rb");
      g_free (path);
      if (!f)
        continue;
      unsigned char ident[5];
      if (fread (ident, 1, sizeof ident, f) == sizeof ident && memcmp (ident, "\177ELF", 4) == 0)
        {
          if (ident[4] == 1)
            *has32 = TRUE;
          else if (ident[4] == 2)
            *has64 = TRUE;
        }
      fclose (f);
    }
}

void
gog_fix_command_free (GogFixCommand *fix)
{
  if (!fix)
    return;
  g_free (fix->systems);
  g_free (fix->command);
  g_free (fix->note);
  g_free (fix);
}

static GogFixCommand *
fix_new (const char *systems, gchar *command, const char *note)
{
  GogFixCommand *fix = g_new0 (GogFixCommand, 1);
  fix->systems = g_strdup (systems);
  fix->command = command;
  fix->note = g_strdup (note);
  return fix;
}

/* TRUE if /etc/os-release names this family in ID or ID_LIKE. */
static gboolean
os_is_like (const char *const *ids)
{
  gchar *text = NULL;
  if (!g_file_get_contents ("/etc/os-release", &text, NULL, NULL))
    return FALSE;
  gboolean found = FALSE;
  gchar **lines = g_strsplit (text, "\n", -1);
  for (gchar **l = lines; *l && !found; l++)
    {
      const char *value = NULL;
      if (g_str_has_prefix (*l, "ID="))
        value = *l + 3;
      else if (g_str_has_prefix (*l, "ID_LIKE="))
        value = *l + 8;
      if (!value)
        continue;
      gchar *unquoted = g_strdup (value);
      g_strdelimit (unquoted, "\"'", ' ');
      gchar **words = g_strsplit (g_strstrip (unquoted), " ", -1);
      for (gchar **w = words; *w && !found; w++)
        for (const char *const *id = ids; *id; id++)
          if (g_strcmp0 (*w, *id) == 0)
            found = TRUE;
      g_strfreev (words);
      g_free (unquoted);
    }
  g_strfreev (lines);
  g_free (text);
  return found;
}

GPtrArray *
runner_freetype_fixes (gchar **summary_out)
{
  gboolean has32, has64;
  find_freetype (&has32, &has64);
  /* Wine only warns when one is missing; if both look present, the 32-bit
   * one is the usual culprit (GOG installers are 32-bit programs). */
  gboolean need64 = !has64;
  gboolean need32 = !has32 || has64;
  const char *which = need32 && need64 ? "FreeType (32-bit and 64-bit)"
                      : need32         ? "the 32-bit FreeType library"
                                       : "the FreeType library";
  if (summary_out)
    *summary_out = g_strdup_printf (
        "Wine could not load %s, which Windows programs need for their text. Without it, "
        "installers and games may show no text, and some installers stop responding. "
        "GOG installers are 32-bit programs.", which);

  GPtrArray *fixes = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_fix_command_free);
  g_ptr_array_add (fixes, fix_new ("Arch, Manjaro, EndeavourOS",
                                   g_strdup_printf ("sudo pacman -S%s%s",
                                                    need64 ? " freetype2" : "",
                                                    need32 ? " lib32-freetype2" : ""),
                                   need32 ? "32-bit packages need the multilib repository "
                                            "enabled in /etc/pacman.conf."
                                          : NULL));
  g_ptr_array_add (fixes, fix_new ("Debian, Ubuntu, Mint",
                                   g_strdup_printf ("%ssudo apt install%s%s",
                                                    need32 ? "sudo dpkg --add-architecture i386 "
                                                             "&& sudo apt update && "
                                                           : "",
                                                    need64 ? " libfreetype6" : "",
                                                    need32 ? " libfreetype6:i386" : ""),
                                   NULL));
  g_ptr_array_add (fixes, fix_new ("Fedora",
                                   g_strdup_printf ("sudo dnf install%s%s",
                                                    need64 ? " freetype" : "",
                                                    need32 ? " freetype.i686" : ""),
                                   NULL));
  g_ptr_array_add (fixes, fix_new ("openSUSE",
                                   g_strdup_printf ("sudo zypper install%s%s",
                                                    need64 ? " libfreetype6" : "",
                                                    need32 ? " libfreetype6-32bit" : ""),
                                   NULL));

  /* This system's own family first. */
  static const char *const families[][4] = {
    { "arch", "manjaro", "endeavouros", NULL },
    { "debian", "ubuntu", "linuxmint", NULL },
    { "fedora", "rhel", NULL, NULL },
    { "suse", "opensuse", "opensuse-leap", NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS (families); i++)
    if (os_is_like (families[i]))
      {
        if (i > 0)
          {
            gpointer mine = g_ptr_array_steal_index (fixes, i);
            g_ptr_array_insert (fixes, 0, mine);
          }
        break;
      }
  return fixes;
}

gchar **
runner_freetype_help (void)
{
  gchar *summary = NULL;
  GPtrArray *fixes = runner_freetype_fixes (&summary);
  GPtrArray *out = g_ptr_array_new ();
  g_ptr_array_add (out, g_strdup ("Fonts are missing for Windows programs."));
  g_ptr_array_add (out, summary);
  g_ptr_array_add (out, g_strdup ("To fix it, run the command for your system in a terminal:"));
  for (guint i = 0; i < fixes->len; i++)
    {
      GogFixCommand *fix = g_ptr_array_index (fixes, i);
      if (fix->note)
        g_ptr_array_add (out, g_strdup_printf ("  %s: %s   (%s)", fix->systems, fix->command,
                                               fix->note));
      else
        g_ptr_array_add (out, g_strdup_printf ("  %s: %s", fix->systems, fix->command));
    }
  g_ptr_array_add (out, NULL);
  g_ptr_array_unref (fixes);
  return (gchar **) g_ptr_array_free (out, FALSE);
}
