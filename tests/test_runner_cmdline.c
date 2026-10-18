/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/runner.h"
#include "../src/proton_fetch.h"

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

/* Manifest arguments are Windows command lines: backslashes stay. */
static void
test_wine_launch_windows_arguments (void)
{
  GogRunnerInfo *runner = gog_runner_info_new (GOG_RUNNER_WINE, "System Wine", "/usr/bin/wine");
  GogCommand *cmd = runner_build_launch_command (
      runner, "/games/foo", "DOSBOX\\dosbox.exe", "DOSBOX",
      "-conf \"..\\dosbox foo.conf\" -path C:\\Data -q \\\"x\\\" -e \"\" end");

  guint n = g_strv_length (cmd->argv);
  g_assert_cmpuint (n, >=, 10);
  const char *const expected[] = { "-conf", "..\\dosbox foo.conf", "-path", "C:\\Data",
                                   "-q", "\"x\"", "-e", "", "end" };
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++)
    g_assert_cmpstr (cmd->argv[n - G_N_ELEMENTS (expected) + i], ==, expected[i]);

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

/* Proton dies on a missing STEAM_COMPAT_DATA_PATH, so the prefix dir must
 * be created for it -- including under an install dir that does not exist. */
static void
test_proton_prefix_dir_is_created (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-prefix-test-XXXXXX", NULL);
  g_assert_nonnull (tmp);
  gchar *install = g_build_filename (tmp, "Some Game", NULL);
  gchar *prefix = runner_prefix_dir_for_install (GOG_RUNNER_PROTON, install);

  g_assert_false (runner_prefix_is_bootstrapped (GOG_RUNNER_PROTON, install));
  GError *error = NULL;
  g_assert_true (runner_ensure_prefix_dir (GOG_RUNNER_PROTON, install, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_test (prefix, G_FILE_TEST_IS_DIR));
  /* idempotent */
  g_assert_true (runner_ensure_prefix_dir (GOG_RUNNER_PROTON, install, &error));
  g_assert_no_error (error);
  /* An empty prefix dir is not a set-up prefix yet. */
  g_assert_false (runner_prefix_is_bootstrapped (GOG_RUNNER_PROTON, install));

  gchar *c_drive = g_build_filename (prefix, "pfx", "dosdevices", "c:", NULL);
  g_assert_cmpint (g_mkdir_with_parents (c_drive, 0755), ==, 0);
  g_assert_true (runner_prefix_is_bootstrapped (GOG_RUNNER_PROTON, install));

  g_rmdir (c_drive);
  gchar *dosdevices = g_path_get_dirname (c_drive);
  gchar *pfx = g_path_get_dirname (dosdevices);
  g_rmdir (dosdevices);
  g_rmdir (pfx);
  g_rmdir (prefix);
  g_rmdir (install);
  g_rmdir (tmp);
  g_free (pfx);
  g_free (dosdevices);
  g_free (c_drive);
  g_free (prefix);
  g_free (install);
  g_free (tmp);
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

static void
test_proton_names_sort_newest_first (void)
{
  /* Negative: the first name is newer and sorts first. */
  g_assert_cmpint (proton_fetch_compare_newest_first ("GE-Proton11-10-x86_64", "GE-Proton11-9-x86_64"), <, 0);
  g_assert_cmpint (proton_fetch_compare_newest_first ("GE-Proton11-9-x86_64", "GE-Proton10-1"), <, 0);
  g_assert_cmpint (proton_fetch_compare_newest_first ("GE-Proton10-1", "GE-Proton9-27"), <, 0);
  g_assert_cmpint (proton_fetch_compare_newest_first ("GE-Proton9-27", "GE-Proton11-9-x86_64"), >, 0);
}

static void
make_fake_build (const char *tools_dir, const char *name)
{
  gchar *bin_dir = g_build_filename (tools_dir, name, "files", "bin", NULL);
  g_assert_cmpint (g_mkdir_with_parents (bin_dir, 0755), ==, 0);
  gchar *proton = g_build_filename (tools_dir, name, "proton", NULL);
  g_assert_true (g_file_set_contents (proton, "#!/bin/sh\n", -1, NULL));
  gchar *wine = g_build_filename (bin_dir, "wine", NULL);
  g_assert_true (g_file_set_contents (wine, "", -1, NULL));
  g_free (wine);
  g_free (proton);
  g_free (bin_dir);
}

static void
on_remove_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void) source;
  GError *error = NULL;
  g_assert_true (proton_fetch_remove_finish (res, &error));
  g_assert_no_error (error);
  g_main_loop_quit (user_data);
}

static void
test_proton_list_and_remove (void)
{
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  make_fake_build (tools_dir, "GE-Proton10-1");
  make_fake_build (tools_dir, "GE-Proton11-9-x86_64");
  make_fake_build (tools_dir, "GE-Proton11-10-x86_64");

  /* A symlink inside a build must be removed, not followed. */
  gchar *outside = g_build_filename (tools_dir, "..", "keep-me", NULL);
  g_assert_true (g_file_set_contents (outside, "x", -1, NULL));
  gchar *link = g_build_filename (tools_dir, "GE-Proton10-1", "files", "link", NULL);
  g_assert_cmpint (symlink (outside, link), ==, 0);

  GPtrArray *names = proton_fetch_list_installed ();
  g_assert_cmpuint (names->len, ==, 3);
  g_assert_cmpstr (names->pdata[0], ==, "GE-Proton11-10-x86_64");
  g_assert_cmpstr (names->pdata[2], ==, "GE-Proton10-1");
  g_ptr_array_unref (names);

  const char *remove[] = { "GE-Proton10-1", "GE-Proton11-9-x86_64", NULL };
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  proton_fetch_remove_async (remove, NULL, on_remove_done, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  names = proton_fetch_list_installed ();
  g_assert_cmpuint (names->len, ==, 1);
  g_assert_cmpstr (names->pdata[0], ==, "GE-Proton11-10-x86_64");
  g_ptr_array_unref (names);
  gchar *gone = g_build_filename (tools_dir, "GE-Proton10-1", NULL);
  g_assert_false (g_file_test (gone, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_test (outside, G_FILE_TEST_IS_REGULAR));

  g_free (gone);
  g_free (link);
  g_free (outside);
  g_free (tools_dir);
}

int
main (int argc, char *argv[])
{
  /* Isolated XDG dirs: the proton tests write into the data dir. */
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/runner/wine_install_command", test_wine_install_command);
  g_test_add_func ("/runner/wine_install_non_silent", test_wine_install_non_silent_omits_switches);
  g_test_add_func ("/runner/proton_install_command_env", test_proton_install_command_env);
  g_test_add_func ("/runner/wine_launch_command", test_wine_launch_command);
  g_test_add_func ("/runner/wine_launch_windows_arguments", test_wine_launch_windows_arguments);
  g_test_add_func ("/runner/wine_root_dir", test_wine_root_dir_proton_uses_pfx_subdir);
  g_test_add_func ("/runner/proton_prefix_dir", test_proton_prefix_dir_is_created);
  g_test_add_func ("/runner/link_c_game", test_link_c_game_resolves_to_install_dir);
  g_test_add_func ("/proton/names_sort_newest_first", test_proton_names_sort_newest_first);
  g_test_add_func ("/proton/list_and_remove", test_proton_list_and_remove);
  return g_test_run ();
}
