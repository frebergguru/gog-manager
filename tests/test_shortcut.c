/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../src/shortcut.h"

static void
remove_tree (const char *dir)
{
  gchar *argv[] = { (gchar *) "rm", (gchar *) "-rf", (gchar *) dir, NULL };
  g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
}

static void
test_exec_value_escaping (void)
{
  gchar *plain = gog_shortcut_exec_value ("/home/me/GOG Games/mouse/play.sh");
  g_assert_cmpstr (plain, ==, "\"/home/me/GOG Games/mouse/play.sh\"");
  g_free (plain);

  /* $ and " get a backslash inside the quotes, and every backslash is then
   * doubled as a string escape. */
  gchar *odd = gog_shortcut_exec_value ("/g/$x \"q\"/a\\b/play.sh");
  g_assert_cmpstr (odd, ==, "\"/g/\\\\$x \\\\\"q\\\\\"/a\\\\\\\\b/play.sh\"");
  g_free (odd);

  /* A literal % is doubled, or it would read as a field code. */
  gchar *pct = gog_shortcut_exec_value ("/g/100% Juice/play.sh");
  g_assert_cmpstr (pct, ==, "\"/g/100%% Juice/play.sh\"");
  g_free (pct);

  /* A line break would end the Exec= line: it goes in as \n. */
  gchar *nl = gog_shortcut_exec_value ("/g/a\nExec=x/play.sh");
  g_assert_cmpstr (nl, ==, "\"/g/a\\nExec=x/play.sh\"");
  g_assert_null (strchr (nl, '\n'));
  g_free (nl);
}

static void
test_create_and_remove (void)
{
  /* Everything lands under a temporary XDG_DATA_HOME, set in main(). */
  gchar *install = g_dir_make_tmp ("gog-shortcut-game-XXXXXX", NULL);
  gchar *info = g_build_filename (install, "goggame-7.info", NULL);
  g_assert_true (g_file_set_contents (
      info, "{ \"playTasks\": [ { \"type\": \"FileTask\", \"isPrimary\": true, \"path\": \"start.sh\" } ] }",
      -1, NULL));
  gchar *icon = g_build_filename (install, "icon.png", NULL);
  g_assert_true (g_file_set_contents (icon, "png", -1, NULL));

  GogGame *game = gog_game_new ();
  gog_game_set_id (game, 7);
  gog_game_set_title (game, "Seven\nLines");
  gog_game_set_icon_path (game, icon);
  gog_game_set_installed (game, TRUE);
  gog_game_set_install_path (game, install);
  gog_game_set_runner_kind (game, GOG_RUNNER_NATIVE);

  g_assert_false (gog_shortcut_exists (game));
  GError *error = NULL;
  g_assert_true (gog_shortcut_create (game, &error));
  g_assert_no_error (error);
  g_assert_true (gog_shortcut_exists (game));

  gchar *path = gog_shortcut_path (game);
  g_assert_true (g_str_has_suffix (path, "/applications/gog-manager-7.desktop"));
  gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_nonnull (strstr (contents, "Name=Seven Lines\n"));
  gchar *exec_line = g_strdup_printf ("Exec=\"%s/play.sh\"\n", install);
  g_assert_nonnull (strstr (contents, exec_line));
  g_assert_nonnull (strstr (contents, "/gog-manager/icons/7.png\n"));
  g_free (exec_line);
  g_free (contents);

  /* The menu entry starts play.sh, which has to be there. */
  gchar *script = g_build_filename (install, "play.sh", NULL);
  g_assert_true (g_file_test (script, G_FILE_TEST_IS_EXECUTABLE));
  g_free (script);

  /* Valid by the spec, where the validator is installed. */
  gchar *validator = g_find_program_in_path ("desktop-file-validate");
  if (validator)
    {
      gchar *argv[] = { validator, path, NULL };
      gint status = -1;
      g_assert_true (g_spawn_sync (NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL));
      g_assert_true (g_spawn_check_wait_status (status, NULL));
      g_free (validator);
    }

  g_assert_true (gog_shortcut_remove (game, &error));
  g_assert_false (gog_shortcut_exists (game));
  g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
  /* Removing what isn't there is fine. */
  g_assert_true (gog_shortcut_remove (game, NULL));

  g_free (path);
  g_object_unref (game);
  g_free (icon);
  g_free (info);
  remove_tree (install);
  g_free (install);
}

int
main (int argc, char *argv[])
{
  gchar *data_home = g_dir_make_tmp ("gog-shortcut-data-XXXXXX", NULL);
  g_setenv ("XDG_DATA_HOME", data_home, TRUE);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/shortcut/exec-escaping", test_exec_value_escaping);
  g_test_add_func ("/shortcut/create-remove", test_create_and_remove);
  int status = g_test_run ();
  remove_tree (data_home);
  g_free (data_home);
  return status;
}
