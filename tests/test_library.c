/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include "../src/library.h"

static void
on_scan_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  gboolean *done = user_data;
  GError *error = NULL;
  g_assert_true (gog_library_scan_finish (GOG_LIBRARY (source), res, &error));
  g_assert_no_error (error);
  *done = TRUE;
}

static void
write_file (const char *dir, const char *name, const char *contents)
{
  gchar *path = g_build_filename (dir, name, NULL);
  g_assert_true (g_file_set_contents (path, contents, -1, NULL));
  g_free (path);
}

static void
remove_file (const char *dir, const char *name)
{
  gchar *path = g_build_filename (dir, name, NULL);
  g_remove (path);
  g_free (path);
}

static void
test_scan_reads_serials (void)
{
  gchar *root = g_dir_make_tmp ("gog-library-test-XXXXXX", NULL);
  gchar *with = g_build_filename (root, "with_keys", NULL);
  gchar *without = g_build_filename (root, "without_keys", NULL);
  g_assert_cmpint (g_mkdir (with, 0755), ==, 0);
  g_assert_cmpint (g_mkdir (without, 0755), ==, 0);
  write_file (with, "product_with_keys.json", "{\"id\": 1, \"title\": \"With Keys\"}");
  write_file (with, "serials.txt", "Game A:\r\nAAAA-BBBB-CCCC\r\n\r\n");
  write_file (without, "product_without_keys.json", "{\"id\": 2, \"title\": \"No Keys\"}");

  GogLibrary *library = gog_library_new ();
  gboolean done = FALSE;
  gog_library_scan_async (library, root, NULL, on_scan_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);

  GogGame *a = gog_library_find_by_id (library, 1);
  GogGame *b = gog_library_find_by_id (library, 2);
  g_assert_nonnull (a);
  g_assert_nonnull (b);
  g_assert_cmpstr (gog_game_get_serials (a), ==, "Game A:\nAAAA-BBBB-CCCC");
  g_assert_null (gog_game_get_serials (b));

  /* let pending progress idles run before the library goes away */
  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_object_unref (library);

  remove_file (with, "product_with_keys.json");
  remove_file (with, "serials.txt");
  remove_file (without, "product_without_keys.json");
  g_rmdir (with);
  g_rmdir (without);
  g_rmdir (root);
  g_free (with);
  g_free (without);
  g_free (root);
}

static void
test_language_from_installer_name (void)
{
  const char *code = NULL, *name = NULL;
  g_assert_true (gog_library_language_from_installer_name (
      "setup_broken_sword_4_1.1a_(german)_(24519).exe", &code, &name));
  g_assert_cmpstr (code, ==, "de");
  g_assert_cmpstr (name, ==, "Deutsch");
  g_assert_true (gog_library_language_from_installer_name ("foo_(french_64bit)_(1).exe", &code, NULL));
  g_assert_cmpstr (code, ==, "fr");
  g_assert_true (gog_library_language_from_installer_name ("foo_(portuguese_brazilian)_(1).exe", &code, NULL));
  g_assert_cmpstr (code, ==, "br");
  g_assert_false (gog_library_language_from_installer_name ("setup_foo_1.0_(64bit)_(24519).exe", NULL, NULL));
  g_assert_false (gog_library_language_from_installer_name ("setup_foo_1.0_(24519).exe", NULL, NULL));
}

/* English and Spanish metadata entries share a size (as in real GOG data),
 * and the German installer has no metadata entry at all. */
static void
test_scan_detects_installer_languages (void)
{
  gchar *root = g_dir_make_tmp ("gog-library-test-XXXXXX", NULL);
  gchar *game = g_build_filename (root, "game", NULL);
  g_assert_cmpint (g_mkdir (game, 0755), ==, 0);
  write_file (game, "product_game.json",
              "{\"id\": 3, \"title\": \"Lang Game\", \"downloads\": {\"installers\": ["
              "{\"os\": \"windows\", \"language\": \"es\", \"language_full\": \"español\", \"total_size\": 4},"
              "{\"os\": \"windows\", \"language\": \"en\", \"language_full\": \"English\", \"total_size\": 4}"
              "]}}");
  const char *files[] = { "setup_game_1.0_(1).exe", "setup_game_1.0_(spanish)_(1).exe",
                          "setup_game_1.0_(german)_(1).exe" };
  for (gsize i = 0; i < G_N_ELEMENTS (files); i++)
    write_file (game, files[i], "MZ..");

  GogLibrary *library = gog_library_new ();
  gboolean done = FALSE;
  gog_library_scan_async (library, root, NULL, on_scan_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);

  GogGame *g = gog_library_find_by_id (library, 3);
  g_assert_nonnull (g);
  GStrv names = gog_game_dup_installer_languages (g);
  GStrv codes = gog_game_dup_installer_language_codes (g);
  g_assert_cmpuint (g_strv_length (names), ==, 3);
  g_assert_cmpstr (names[0], ==, "Deutsch");
  g_assert_cmpstr (names[1], ==, "English");
  g_assert_cmpstr (names[2], ==, "español");
  g_assert_cmpstr (codes[0], ==, "de");
  g_assert_cmpstr (codes[1], ==, "en");
  g_assert_cmpstr (codes[2], ==, "es");
  g_strfreev (names);
  g_strfreev (codes);

  GStrv all = gog_library_get_all_languages (library);
  g_assert_true (g_strv_contains ((const char *const *) all, "Deutsch"));
  g_strfreev (all);

  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_object_unref (library);

  remove_file (game, "product_game.json");
  for (gsize i = 0; i < G_N_ELEMENTS (files); i++)
    remove_file (game, files[i]);
  g_rmdir (game);
  g_rmdir (root);
  g_free (game);
  g_free (root);
}

/* Every game lists English and most list Deutsch; each language must still
 * appear once in the menu, sorted. */
static void
test_all_languages_listed_once (void)
{
  gchar *root = g_dir_make_tmp ("gog-library-test-XXXXXX", NULL);
  const char *dirs[] = { "a", "b", "c" };
  const char *jsons[] = {
    "{\"id\": 11, \"title\": \"A\", \"languages\": {\"en\": \"English\", \"de\": \"Deutsch\"}}",
    "{\"id\": 12, \"title\": \"B\", \"languages\": {\"de\": \"Deutsch\", \"en\": \"English\", \"fr\": \"français\"}}",
    "{\"id\": 13, \"title\": \"C\", \"languages\": {\"en\": \"English\"}}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
      gchar *dir = g_build_filename (root, dirs[i], NULL);
      g_assert_cmpint (g_mkdir (dir, 0755), ==, 0);
      write_file (dir, "product_x.json", jsons[i]);
      /* Untagged installer: English, already in the metadata list. */
      write_file (dir, "setup_x_(1).exe", "MZ");
      g_free (dir);
    }

  GogLibrary *library = gog_library_new ();
  gboolean done = FALSE;
  gog_library_scan_async (library, root, NULL, on_scan_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);

  GStrv all = gog_library_get_all_languages (library);
  g_assert_cmpuint (g_strv_length (all), ==, 3);
  g_assert_cmpstr (all[0], ==, "Deutsch");
  g_assert_cmpstr (all[1], ==, "English");
  g_assert_cmpstr (all[2], ==, "français");
  g_strfreev (all);

  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_object_unref (library);

  for (gsize i = 0; i < G_N_ELEMENTS (dirs); i++)
    {
      gchar *dir = g_build_filename (root, dirs[i], NULL);
      remove_file (dir, "product_x.json");
      remove_file (dir, "setup_x_(1).exe");
      g_rmdir (dir);
      g_free (dir);
    }
  g_rmdir (root);
  g_free (root);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/library/scan_reads_serials", test_scan_reads_serials);
  g_test_add_func ("/library/language_from_installer_name", test_language_from_installer_name);
  g_test_add_func ("/library/scan_detects_installer_languages", test_scan_detects_installer_languages);
  g_test_add_func ("/library/all_languages_listed_once", test_all_languages_listed_once);
  return g_test_run ();
}
