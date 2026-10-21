/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../src/launch.h"

static const char *INFO =
  "{ \"gameId\": \"42\", \"playTasks\": ["
  "  { \"type\": \"FileTask\", \"isPrimary\": true, \"category\": \"game\","
  "    \"path\": \"bin\\\\Game.exe\", \"workingDir\": \"bin\", \"arguments\": \"-windowed\" } ] }";

static gchar *
make_install_dir (void)
{
  gchar *dir = g_dir_make_tmp ("gog-play-script-XXXXXX", NULL);
  g_assert_nonnull (dir);
  gchar *info = g_build_filename (dir, "goggame-42.info", NULL);
  g_assert_true (g_file_set_contents (info, INFO, -1, NULL));
  g_free (info);
  return dir;
}

static GogGame *
make_game (const char *install_dir, GogRunnerKind kind, const char *runner_path)
{
  GogGame *game = gog_game_new ();
  gog_game_set_id (game, 42);
  gog_game_set_title (game, "Test Game");
  gog_game_set_installed (game, TRUE);
  gog_game_set_install_path (game, install_dir);
  gog_game_set_runner_kind (game, kind);
  gog_game_set_runner_path (game, runner_path);
  return game;
}

static gchar *
read_script (const char *install_dir)
{
  gchar *path = g_build_filename (install_dir, "play.sh", NULL);
  g_assert_true (g_file_test (path, G_FILE_TEST_IS_EXECUTABLE));
  gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));

  /* It has to be valid shell. */
  gchar *argv[] = { (gchar *) "sh", (gchar *) "-n", path, NULL };
  gint status = -1;
  g_assert_true (g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL,
                               &status, NULL));
  g_assert_true (g_spawn_check_wait_status (status, NULL));
  g_free (path);
  return contents;
}

static void
remove_dir (const char *dir)
{
  gchar *argv[] = { (gchar *) "rm", (gchar *) "-rf", (gchar *) dir, NULL };
  g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
}

static void
test_proton_script (void)
{
  gchar *dir = make_install_dir ();
  GogGame *game = make_game (dir, GOG_RUNNER_PROTON, "/opt/GE-Proton10-34");
  GError *error = NULL;
  g_assert_true (gog_write_play_script (game, &error));
  g_assert_no_error (error);

  gchar *s = read_script (dir);
  g_assert_true (g_str_has_prefix (s, "#!/bin/sh\n# Written by gog-manager"));
  g_assert_nonnull (strstr (s, "export STEAM_COMPAT_DATA_PATH=\"$here/proton-prefix\""));
  g_assert_nonnull (strstr (s, "exec '/opt/GE-Proton10-34/proton' 'run' 'start' '/b' '/wait' "
                               "'/d' 'c:\\game\\bin' 'c:\\game\\bin\\Game.exe' '-windowed' \"$@\""));
  /* Nothing points at where the game happens to be installed. */
  g_assert_null (strstr (s, dir));
  g_free (s);

  g_object_unref (game);
  remove_dir (dir);
  g_free (dir);
}

static void
test_wine_script (void)
{
  gchar *dir = make_install_dir ();
  GogGame *game = make_game (dir, GOG_RUNNER_WINE, "/usr/bin/wine");
  g_assert_true (gog_write_play_script (game, NULL));

  gchar *s = read_script (dir);
  g_assert_nonnull (strstr (s, "export WINEPREFIX=\"$root\""));
  g_assert_nonnull (strstr (s, "root=\"$here/prefix\""));
  g_assert_nonnull (strstr (s, "exec '/usr/bin/wine' 'start'"));
  g_free (s);

  g_object_unref (game);
  remove_dir (dir);
  g_free (dir);
}

static void
test_native_script (void)
{
  gchar *dir = make_install_dir ();
  GogGame *game = make_game (dir, GOG_RUNNER_NATIVE, NULL);
  g_assert_true (gog_write_play_script (game, NULL));

  gchar *s = read_script (dir);
  g_assert_nonnull (strstr (s, "cd \"$here\"/'bin' || exit 1\nexec \"$here\"/'bin/Game.exe' '-windowed' \"$@\"\n"));
  g_free (s);

  g_object_unref (game);
  remove_dir (dir);
  g_free (dir);
}

static void
test_rewrites_own_script_but_not_others (void)
{
  gchar *dir = make_install_dir ();
  GogGame *game = make_game (dir, GOG_RUNNER_WINE, "/usr/bin/wine");
  g_assert_true (gog_write_play_script (game, NULL));

  /* A runner change rewrites gog-manager's own script. */
  gog_game_set_runner_path (game, "/opt/wine-staging/bin/wine");
  g_assert_true (gog_write_play_script (game, NULL));
  gchar *s = read_script (dir);
  g_assert_nonnull (strstr (s, "'/opt/wine-staging/bin/wine'"));
  g_free (s);

  /* One the user wrote stays as it is. */
  gchar *path = g_build_filename (dir, "play.sh", NULL);
  g_assert_true (g_file_set_contents (path, "#!/bin/sh\necho mine\n", -1, NULL));
  GError *error = NULL;
  g_assert_false (gog_write_play_script (game, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  gchar *contents = NULL;
  g_assert_true (g_file_get_contents (path, &contents, NULL, NULL));
  g_assert_cmpstr (contents, ==, "#!/bin/sh\necho mine\n");
  g_free (contents);
  g_free (path);

  g_object_unref (game);
  remove_dir (dir);
  g_free (dir);
}

typedef struct
{
  guint reports;
  gint64 game_id;
  gint64 seconds;
} Played;

static void
on_played (gint64 game_id, gint64 started, gint64 seconds, gpointer user_data)
{
  Played *p = user_data;
  g_assert_cmpint (started, >, 0);
  p->reports++;
  p->game_id = game_id;
  p->seconds = seconds;
}

/* A launch reports the start, and how long the game ran when it ends. */
static void
test_play_session (void)
{
  gchar *dir = g_dir_make_tmp ("gog-play-session-XXXXXX", NULL);
  gchar *game_bin = g_build_filename (dir, "bin", NULL);
  g_assert_cmpint (g_mkdir (game_bin, 0755), ==, 0);
  gchar *exe = g_build_filename (game_bin, "game", NULL);
  g_assert_true (g_file_set_contents (exe, "#!/bin/sh\nsleep 1\n", -1, NULL));
  gchar *info = g_build_filename (dir, "goggame-42.info", NULL);
  g_assert_true (g_file_set_contents (info,
                                      "{ \"gameId\": \"42\", \"playTasks\": [ { \"type\": \"FileTask\","
                                      " \"isPrimary\": true, \"category\": \"game\", \"path\": \"bin/game\","
                                      " \"workingDir\": \"bin\" } ] }",
                                      -1, NULL));
  GogGame *game = make_game (dir, GOG_RUNNER_NATIVE, NULL);

  Played played = { 0 };
  gog_launch_set_played_func (on_played, &played);
  g_assert_true (gog_launch_game (game, NULL));
  while (played.reports < 2)
    g_main_context_iteration (NULL, TRUE);
  g_assert_cmpint (played.game_id, ==, 42);
  g_assert_cmpint (played.seconds, >=, 1);

  /* One still running when GOG Manager quits counts until then, once. */
  played.reports = 0;
  g_assert_true (gog_launch_game (game, NULL));
  while (played.reports < 1)
    g_main_context_iteration (NULL, TRUE);
  gog_launch_finish_sessions ();
  g_assert_cmpuint (played.reports, ==, 2);
  g_usleep (1500 * 1000);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_assert_cmpuint (played.reports, ==, 2);
  gog_launch_set_played_func (NULL, NULL);

  g_object_unref (game);
  remove_dir (dir);
  g_free (info);
  g_free (exe);
  g_free (game_bin);
  g_free (dir);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/play-script/proton", test_proton_script);
  g_test_add_func ("/play-script/wine", test_wine_script);
  g_test_add_func ("/play-script/native", test_native_script);
  g_test_add_func ("/play-script/keeps-foreign-script", test_rewrites_own_script_but_not_others);
  g_test_add_func ("/launch/play-session", test_play_session);
  return g_test_run ();
}
