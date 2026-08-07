#include "runner.h"
#include "proton_fetch.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>

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

  gchar *link_path = g_build_filename (dosdevices, "c:", NULL);
  /* g_build_filename collapses "c:" fine since it's just a path segment. */
  g_free (dosdevices);

  gchar *game_link = g_build_filename (link_path, "game", NULL);
  g_free (link_path);

  if (g_file_test (game_link, G_FILE_TEST_EXISTS) || g_file_test (game_link, G_FILE_TEST_IS_SYMLINK))
    {
      /* Verify symlink is valid; if it exists but points elsewhere, fail. */
      gchar link_target[PATH_MAX];
      ssize_t len = readlink (game_link, link_target, sizeof (link_target) - 1);
      if (len > 0)
        {
          link_target[len] = '\0';
          g_free (game_link);
          return TRUE;
        }
      /* Symlink exists but readlink failed, or is not a symlink - remove it */
      g_remove (game_link);
    }

  /* link_dir (".../prefix/dosdevices/c") is always a descendant of
   * install_dir by construction (the prefix lives under install_dir), so
   * compute install_dir's path *down to* link_dir and turn each segment
   * into a "../" to build the reverse (link_dir up to install_dir). */
  gchar *link_dir = g_path_get_dirname (game_link);
  gchar *relative = NULL;
  {
    GFile *install_gfile = g_file_new_for_path (install_dir);
    GFile *link_gfile = g_file_new_for_path (link_dir);
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
    else
      {
        /* Should not happen given how wine_root is always built under
         * install_dir, but fall back to an absolute target if it ever does. */
        relative = g_strdup (install_dir);
      }
    g_object_unref (install_gfile);
    g_object_unref (link_gfile);
  }
  g_free (link_dir);

  gboolean ok = (symlink (relative, game_link) == 0);
  if (!ok)
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "Could not symlink %s -> %s", game_link, relative);

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
    {
      gint argc = 0;
      gchar **split = NULL;
      GError *parse_error = NULL;
      if (!g_shell_parse_argv (arguments, &argc, &split, &parse_error))
        {
          g_warning ("Failed to parse command arguments: %s", parse_error->message);
          g_error_free (parse_error);
        }
      else
        {
          for (gint i = 0; i < argc; i++)
            g_ptr_array_add (argv, split[i]);
          g_free (split); /* elements now owned by argv */
        }
    }

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
