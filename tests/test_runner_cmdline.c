#include <glib.h>
#include <string.h>
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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/runner/wine_install_command", test_wine_install_command);
  g_test_add_func ("/runner/wine_install_non_silent", test_wine_install_non_silent_omits_switches);
  g_test_add_func ("/runner/proton_install_command_env", test_proton_install_command_env);
  g_test_add_func ("/runner/wine_launch_command", test_wine_launch_command);
  g_test_add_func ("/runner/wine_root_dir", test_wine_root_dir_proton_uses_pfx_subdir);
  return g_test_run ();
}
