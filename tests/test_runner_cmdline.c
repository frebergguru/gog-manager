/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/runner.h"

static gboolean
argv_contains (gchar **argv, const char *needle)
{
  for (gchar **p = argv; p && *p; p++)
    if (g_strcmp0 (*p, needle) == 0)
      return TRUE;
  return FALSE;
}

static const char *
envp_lookup (gchar **envp, const char *key)
{
  gsize keylen = strlen (key);
  for (gchar **p = envp; p && *p; p++)
    if (strncmp (*p, key, keylen) == 0 && (*p)[keylen] == '=')
      return *p + keylen + 1;
  return NULL;
}

static void
test_wine_install_command (void)
{
  GogRunnerInfo *runner = gog_runner_info_new (GOG_RUNNER_WINE, "System Wine", "/usr/bin/wine");
  GogCommand *cmd = runner_build_install_command (runner, "/games/foo/setup_foo.exe",
                                                   "/games/foo", "en", TRUE);

  g_assert_cmpstr (cmd->argv[0], ==, "/usr/bin/wine");
  g_assert_true (argv_contains (cmd->argv, "/games/foo/setup_foo.exe"));
  g_assert_true (argv_contains (cmd->argv, "/DIR=c:\\game"));
  g_assert_true (argv_contains (cmd->argv, "/LANG=en"));
  g_assert_true (argv_contains (cmd->argv, "/SILENT"));
  g_assert_true (argv_contains (cmd->argv, "/SUPPRESSMSGBOXES"));

  const char *prefix = envp_lookup (cmd->envp, "WINEPREFIX");
  g_assert_nonnull (prefix);
  g_assert_true (g_str_has_suffix (prefix, "/games/foo/prefix"));

  gog_command_free (cmd);
  gog_runner_info_free (runner);
}

static void
test_wine_install_non_silent_omits_switches (void)
{
  GogRunnerInfo *runner = gog_runner_info_new (GOG_RUNNER_WINE, "System Wine", "/usr/bin/wine");
  GogCommand *cmd = runner_build_install_command (runner, "/games/foo/setup_foo.exe",
                                                   "/games/foo", "en", FALSE);
  g_assert_false (argv_contains (cmd->argv, "/SILENT"));
  g_assert_false (argv_contains (cmd->argv, "/SUPPRESSMSGBOXES"));
  g_assert_true (argv_contains (cmd->argv, "/DIR=c:\\game")); /* still targets the right dir */
  gog_command_free (cmd);
  gog_runner_info_free (runner);
}

static void
test_proton_install_command_env (void)
{
  GogRunnerInfo *runner = gog_runner_info_new (GOG_RUNNER_PROTON, "GE-Proton9-20",
                                                "/home/user/.config/heroic/tools/proton/GE-Proton9-20");
  GogCommand *cmd =
      runner_build_install_command (runner, "/games/bar/setup_bar.exe", "/games/bar", "en", TRUE);

  g_assert_true (g_str_has_suffix (cmd->argv[0], "/GE-Proton9-20/proton"));
  g_assert_cmpstr (cmd->argv[1], ==, "run");

  const char *data_path = envp_lookup (cmd->envp, "STEAM_COMPAT_DATA_PATH");
  g_assert_nonnull (data_path);
  g_assert_true (g_str_has_suffix (data_path, "/games/bar/proton-prefix"));
  g_assert_nonnull (envp_lookup (cmd->envp, "STEAM_COMPAT_CLIENT_INSTALL_PATH"));

  gog_command_free (cmd);
  gog_runner_info_free (runner);
}

static void
test_wine_launch_command (void)
{
  GogRunnerInfo *runner = gog_runner_info_new (GOG_RUNNER_WINE, "System Wine", "/usr/bin/wine");
  GogCommand *cmd = runner_build_launch_command (runner, "/games/foo", "Game/foo.exe", "Game",
                                                  "--fullscreen");

  g_assert_cmpstr (cmd->argv[0], ==, "/usr/bin/wine");
  g_assert_true (argv_contains (cmd->argv, "start"));
  g_assert_true (argv_contains (cmd->argv, "c:\\game\\Game\\foo.exe"));
  g_assert_true (argv_contains (cmd->argv, "c:\\game\\Game"));
  g_assert_true (argv_contains (cmd->argv, "--fullscreen"));

  gog_command_free (cmd);
  gog_runner_info_free (runner);
}

static void
test_wine_root_dir_proton_uses_pfx_subdir (void)
{
  gchar *wine_root = runner_wine_root_dir (GOG_RUNNER_PROTON, "/games/baz");
  g_assert_true (g_str_has_suffix (wine_root, "/games/baz/proton-prefix/pfx"));
  g_free (wine_root);

  gchar *wine_root2 = runner_wine_root_dir (GOG_RUNNER_WINE, "/games/baz");
  g_assert_true (g_str_has_suffix (wine_root2, "/games/baz/prefix"));
  g_assert_false (g_str_has_suffix (wine_root2, "pfx"));
  g_free (wine_root2);
}

/* Wine makes dosdevices/c: a symlink to ../drive_c, so c:/game must resolve
 * back to the install dir through that link, not through the lexical path. */
static void
test_link_c_game_resolves_to_install_dir (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-link-test-XXXXXX", NULL);
  g_assert_nonnull (tmp);
  gchar *install = g_build_filename (tmp, "Some Game", NULL);
  gchar *wine_root = runner_wine_root_dir (GOG_RUNNER_WINE, install);
  gchar *drive_c = g_build_filename (wine_root, "drive_c", NULL);
  gchar *dosdevices = g_build_filename (wine_root, "dosdevices", NULL);
  gchar *c_link = g_build_filename (dosdevices, "c:", NULL);
  g_assert_cmpint (g_mkdir_with_parents (drive_c, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (dosdevices, 0755), ==, 0);
  g_assert_cmpint (symlink ("../drive_c", c_link), ==, 0);

  /* A wrong link left by an older version must be repaired. */
  gchar *game_link = g_build_filename (drive_c, "game", NULL);
  g_assert_cmpint (symlink ("../../../", game_link), ==, 0);

  GError *error = NULL;
  g_assert_true (runner_link_c_game (wine_root, install, &error));
  g_assert_no_error (error);
  /* idempotent */
  g_assert_true (runner_link_c_game (wine_root, install, &error));
  g_assert_no_error (error);

  gchar *via_link = g_build_filename (c_link, "game", NULL);
  char *resolved = realpath (via_link, NULL);
  char *expected = realpath (install, NULL);
  g_assert_cmpstr (resolved, ==, expected);
  free (resolved);
  free (expected);

  g_remove (game_link);
  g_remove (c_link);
  g_rmdir (dosdevices);
  g_rmdir (drive_c);
  g_rmdir (wine_root);
  g_rmdir (install);
  g_rmdir (tmp);
  g_free (via_link);
  g_free (game_link);
  g_free (c_link);
  g_free (dosdevices);
  g_free (drive_c);
  g_free (wine_root);
  g_free (install);
  g_free (tmp);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/runner/wine_install_command", test_wine_install_command);
  g_test_add_func ("/runner/wine_install_non_silent", test_wine_install_non_silent_omits_switches);
  g_test_add_func ("/runner/proton_install_command_env", test_proton_install_command_env);
  g_test_add_func ("/runner/wine_launch_command", test_wine_launch_command);
  g_test_add_func ("/runner/wine_root_dir", test_wine_root_dir_proton_uses_pfx_subdir);
  g_test_add_func ("/runner/link_c_game", test_link_c_game_resolves_to_install_dir);
  return g_test_run ();
}
