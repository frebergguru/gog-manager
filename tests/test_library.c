/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include "../src/library.h"
#include "../src/gog_sync.h"
#include "../src/original_language.h"
#include "../src/installer_engine.h"
#include "../src/config.h"

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
  write_file (with, "product_with_keys.json",
              "{\"id\": 1, \"title\": \"With Keys\", \"release_date\": \"2021-09-29T17:50:00+0200\"}");
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
  /* GOG writes the offset without a colon. */
  g_assert_cmpint (gog_game_get_release_date (a), ==, 1632930600);
  g_assert_cmpint (gog_game_get_release_date (b), ==, 0);

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

/* The rest of the product json: changelog, links, screenshots, videos. */
static void
test_scan_reads_product_info (void)
{
  gchar *root = g_dir_make_tmp ("gog-library-test-XXXXXX", NULL);
  gchar *dir = g_build_filename (root, "info", NULL);
  g_assert_cmpint (g_mkdir (dir, 0755), ==, 0);
  write_file (dir, "game-details.json",
              "{\"cdKey\": \"<span>Doom 3:</span><span>AAAA BB</span>\","
              " \"messages\": [\"<p>Claim it at <a href=\\\"https://example.com/x?a=1&b=2\\\">the site</a>.</p>\"]}");
  write_file (dir, "product_info.json",
              "{\"id\": 5, \"title\": \"Info\","
              " \"description\": {\"lead\": \"<p>Short.</p>\"},"
              " \"changelog\": \"<h4>1.1</h4><ul><li>Fix</li></ul>\","
              " \"in_development\": {\"active\": true, \"until\": null},"
              " \"links\": {\"product_card\": \"https://www.gog.com/game/info\","
              "             \"forum\": \"http://example.com/forum\"},"
              " \"screenshots\": [{\"image_id\": \"eb3e88\"}, {\"image_id\": \"../x\"}],"
              " \"videos\": [{\"provider\": \"youtube\","
              "               \"video_url\": \"https://www.youtube.com/embed/B4J3TdI55MA?wmode=opaque\"},"
              "              {\"provider\": \"vimeo\", \"video_url\": \"https://vimeo.com/1\"}]}");
  GogLibrary *library = gog_library_new ();
  gboolean done = FALSE;
  gog_library_scan_async (library, root, NULL, on_scan_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);

  GogGame *g = gog_library_find_by_id (library, 5);
  g_assert_nonnull (g);
  const GogProductInfo *info = gog_game_get_product_info (g);
  g_assert_cmpstr (info->lead, ==, "Short.");
  g_assert_cmpstr (info->changelog, ==,
                   "<span size=\"large\" weight=\"bold\">1.1</span>\n\xE2\x80\xA2 Fix");
  g_assert_true (info->in_development);
  g_assert_cmpstr (info->store_url, ==, "https://www.gog.com/game/info");
  /* Only GOG's own pages. */
  g_assert_null (info->forum_url);
  g_assert_null (info->support_url);
  g_assert_cmpuint (g_strv_length (info->screenshots), ==, 1);
  g_assert_cmpstr (info->screenshots[0], ==, "https://images.gog-statics.com/eb3e88{formatter}.{ext}");
  g_assert_cmpuint (g_strv_length (info->video_ids), ==, 1);
  g_assert_cmpstr (info->video_ids[0], ==, "B4J3TdI55MA");
  /* game-details.json: GOG's messages with their links, and the CD keys
   * when there's no serials.txt. */
  g_assert_cmpuint (g_strv_length (info->messages), ==, 1);
  g_assert_cmpstr (info->messages[0], ==,
                   "Claim it at <a href=\"https://example.com/x?a=1&amp;b=2\">the site</a>.");
  g_assert_cmpstr (gog_game_get_serials (g), ==, "Doom 3:\nAAAA BB");

  while (g_main_context_iteration (NULL, FALSE))
    ;
  g_object_unref (library);
  remove_file (dir, "product_info.json");
  remove_file (dir, "game-details.json");
  g_rmdir (dir);
  g_rmdir (root);
  g_free (dir);
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

/* Lines as lgogdownloader --status --size-only prints them: ND lines have
 * no size, file names may contain spaces, errors are mixed in, and a game's
 * DLC lines follow the game's own. */
static void
test_gog_sync_parse_status (void)
{
  gchar *root = g_dir_make_tmp ("gog-library-test-XXXXXX", NULL);
  gchar *have = g_build_filename (root, "fallout_4", NULL);
  gchar *cyberpunk = g_build_filename (root, "cyberpunk_2077", NULL);
  g_assert_cmpint (g_mkdir (have, 0755), ==, 0);
  g_assert_cmpint (g_mkdir (cyberpunk, 0755), ==, 0);

  const char *output =
      "OK broken_sword_4 setup_broken_sword_4_(24519).exe 832336\n"
      "HTTP ERROR: 404 (https://www.gog.com/account/gameDetails/1207664643.json)\n"
      "OK cyberpunk_2077 setup_cyberpunk_2077.exe 1234\n"
      "ND cyberpunk_2077_goodies cp77_pl_ key_visual_graphics.zip\n"
      "FS fallout_4 setup_fallout_4_(portuguese_brazilian_64bit)_(66700)-4.bin 1174016000 "
      "0123456789abcdef0123456789abcdef\n"
      "ND fallout_4 setup_fallout_4_(german_64bit)_(66700).exe\n"
      "ND new_game setup_new_game_1.0_(1).exe\n"
      "ND new_game new_game_manual.zip\n";
  const char *base_names[] = { "broken_sword_4", "cyberpunk_2077", "fallout_4", "new_game", NULL };
  GPtrArray *items = gog_sync_parse_status (output, root, base_names);
  g_assert_cmpuint (items->len, ==, 3);

  GogSyncItem *a = g_ptr_array_index (items, 0);
  g_assert_cmpstr (a->gamename, ==, "cyberpunk_2077"); /* the DLC's game */
  g_assert_cmpint (a->kind, ==, GOG_SYNC_UPDATE);
  g_assert_cmpuint (a->n_files, ==, 1);
  GogSyncItem *b = g_ptr_array_index (items, 1);
  g_assert_cmpstr (b->gamename, ==, "fallout_4");
  g_assert_cmpint (b->kind, ==, GOG_SYNC_UPDATE);
  g_assert_cmpuint (b->n_files, ==, 2);
  GogSyncItem *c = g_ptr_array_index (items, 2);
  g_assert_cmpstr (c->gamename, ==, "new_game");
  g_assert_cmpint (c->kind, ==, GOG_SYNC_NEW_GAME);
  g_assert_cmpuint (c->n_files, ==, 2);

  /* Sizes and hashes are not part of the name; spaces are. */
  GogSyncFile *dlc = g_ptr_array_index (a->files, 0);
  g_assert_cmpstr (dlc->gamename, ==, "cyberpunk_2077_goodies");
  g_assert_cmpstr (dlc->filename, ==, "cp77_pl_ key_visual_graphics.zip");
  g_assert_cmpint (dlc->part, ==, GOG_SYNC_PART_DLC);
  GogSyncFile *fs = g_ptr_array_index (b->files, 0);
  g_assert_cmpstr (fs->filename, ==, "setup_fallout_4_(portuguese_brazilian_64bit)_(66700)-4.bin");
  g_assert_cmpint (fs->part, ==, GOG_SYNC_PART_GAME);

  /* --list j: what each file is, its language and size. */
  const char *details =
      "Getting game names 4 / 4\n"
      "[\n"
      "{ \"gamename\": \"fallout_4\", \"title\": \"Fallout 4\", \"changelog\": \"<h4>1.1</h4>\",\n"
      "  \"installers\": [\n"
      "    { \"gamename\": \"fallout_4\", \"language\": 2, \"size\": \"5000\", \"type\": 1,\n"
      "      \"path\": \"/fallout_4/setup_fallout_4_(german_64bit)_(66700).exe\" },\n"
      "    { \"gamename\": \"fallout_4\", \"language\": 1, \"size\": \"7\", \"type\": 1,\n"
      "      \"path\": \"/fallout_4/setup_fallout_4.exe\" } ],\n"
      "  \"dlcs\": [] },\n"
      "{ \"gamename\": \"new_game\", \"title\": \"New Game\", \"product_id\": \"1207\",\n"
      "  \"installers\": [], \"extras\": [\n"
      "    { \"gamename\": \"new_game\", \"language\": 1, \"size\": \"300\", \"type\": 2,\n"
      "      \"name\": \"Manual\", \"path\": \"/new_game/new_game_manual.zip\" } ],\n"
      "  \"dlcs\": [ { \"gamename\": \"new_game_dlc\", \"title\": \"Big DLC\",\n"
      "    \"installers\": [ { \"gamename\": \"new_game_dlc\", \"language\": 65536,\n"
      "      \"size\": \"9\", \"type\": 16, \"path\": \"/new_game_dlc/setup_dlc.exe\" } ] } ] }\n"
      "]\n";
  gog_sync_apply_details (items, details);
  g_assert_cmpuint (b->languages_offered, ==, 3);
  GogSyncFile *german = g_ptr_array_index (b->files, 1);
  g_assert_cmpuint (german->languages, ==, 2);
  g_assert_cmpuint (german->size, ==, 5000);
  g_assert_false (gog_sync_file_wanted (german, 1));
  g_assert_true (gog_sync_file_wanted (german, 0));
  g_assert_cmpuint (c->languages_offered, ==, 65536);
  GogSyncFile *manual = g_ptr_array_index (c->files, 1);
  g_assert_cmpint (manual->part, ==, GOG_SYNC_PART_EXTRA);
  g_assert_cmpstr (manual->label, ==, "Manual");
  g_assert_true (gog_sync_file_wanted (manual, 2)); /* extras have no language */
  g_assert_cmpstr (c->title, ==, "New Game");
  g_assert_cmpint (c->product_id, ==, 1207);
  g_assert_cmpint (b->product_id, ==, 0);
  g_assert_cmpstr (b->changelog, ==, "<h4>1.1</h4>");
  g_assert_null (c->changelog);

  /* Owned but not downloaded: a game of its own in the library, once. */
  GogLibrary *library = gog_library_new ();
  gog_library_set_not_downloaded (library, items);
  gog_library_set_not_downloaded (library, items);
  GListModel *model = gog_library_get_model (library);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 1);
  GogGame *remote = gog_library_find_by_id (library, 1207);
  g_assert_nonnull (remote);
  g_assert_false (gog_game_get_downloaded (remote));
  g_assert_cmpstr (gog_game_get_title (remote), ==, "New Game");
  g_assert_cmpstr (gog_game_get_slug (remote), ==, "new_game");
  /* Downloaded since: gone again. */
  c->kind = GOG_SYNC_UPDATE;
  gog_library_set_not_downloaded (library, items);
  g_assert_cmpuint (g_list_model_get_n_items (model), ==, 0);
  g_object_unref (library);
  g_ptr_array_unref (items);

  gchar *langs = gog_sync_describe_languages (1 | 65536);
  g_assert_cmpstr (langs, ==, "English, Norwegian");
  g_free (langs);

  gchar *notes = gog_sync_describe_notifications ("New forum replies: 2\nUpdated games: 0\n"
                                                  "Unread chat messages: 1\n"
                                                  "Pending friend requests: 0\n");
  g_assert_cmpstr (notes, ==, "2 new forum replies · 1 unread chat message");
  g_free (notes);
  g_assert_null (gog_sync_describe_notifications ("New forum replies: 0\nUpdated games: 0\n"));
  g_assert_null (gog_sync_describe_notifications (NULL));

  /* lgogdownloader --repair, as it printed for Truxton. */
  gchar *repair = gog_sync_describe_repair (
      "\r\rGetting game info 1 / 1\n"
      "Repairing file /games/truxton/setup_truxton_36_(64bit)_(72706).exe\n"
      "XML: Using remote file\nXML: Valid XML\nXML: Parsing finished\n\n"
      "setup_truxton_36_(64bit)_(72706).exe\n\tMD5:\tb68daf8ede\n\tChunks:\t2\n\n"
      "\rChunk 0 (10485760 bytes): OK\r\rChunk 1 (1857456 bytes): OK\r\n");
  g_assert_cmpstr (repair, ==, "Checked 1 file: everything matches GOG.");
  g_free (repair);
  repair = gog_sync_describe_repair (
      "Repairing file /g/a.exe\nChunk 0 (10 bytes): OK\rChunk 1 (10 bytes): CORRUPTED\n"
      "Repairing file /g/b.bin\nFilesizes don't match\n"
      "Repairing file /g/c.bin\nXML: Failed\n"
      "File doesn't exist: /g/d.bin\n");
  g_assert_cmpstr (repair, ==,
                   "Checked 3 files. 1 damaged part was downloaded again. 1 file is not the size "
                   "GOG has (another version): Check Now in the download window offers the new "
                   "one. GOG has no checksums for 1 file, so it could not be checked. 1 file that "
                   "isn't downloaded (another language or part) was skipped.");
  g_free (repair);
  repair = gog_sync_describe_repair ("");
  g_assert_cmpstr (repair, ==, "No downloaded files of this game were found to check.");
  g_free (repair);

  gchar *pretty = gog_sync_pretty_name ("broken_sword_4");
  g_assert_cmpstr (pretty, ==, "Broken Sword 4");
  g_free (pretty);

  g_rmdir (have);
  g_rmdir (cyberpunk);
  g_rmdir (root);
  g_free (cyberpunk);
  g_free (have);
  g_free (root);
}

#define BIT_EN (1u << 0)
#define BIT_DE (1u << 1)
#define BIT_FR (1u << 2)
#define BIT_NO (1u << 16)

/* Default download languages: each game's original language (cached from
 * Wikidata) plus the extra defaults, English when the original isn't known,
 * and per-game choices before all of it. */
static void
test_default_download_languages (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-langs-XXXXXX", NULL);
  /* The app keeps its files in .gog-manager-configs next to the AppImage. */
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE);
  gchar *configs = g_build_filename (tmp, ".gog-manager-configs", NULL);
  g_assert_cmpint (g_mkdir (configs, 0700), ==, 0);
  gchar *cache = g_build_filename (configs, "gog-manager-original-languages.json", NULL);
  g_assert_true (g_file_set_contents (
      cache,
      "{\"among_the_sleep\": {\"languages\": \"no\", \"source\": \"made in Norway\", "
      "\"checked\": 1}, \"mystery\": {\"languages\": \"\", \"checked\": 1}}",
      -1, NULL));

  g_assert_cmpuint (gog_original_country_languages ("US"), ==, BIT_EN);
  g_assert_cmpuint (gog_original_country_languages ("CA"), ==, BIT_EN | BIT_FR);
  g_assert_cmpuint (gog_original_country_languages ("NO"), ==, BIT_NO);
  g_assert_cmpuint (gog_original_country_languages ("XX"), ==, 0);

  gchar *source = NULL;
  g_assert_cmpuint (gog_original_languages ("among_the_sleep", &source), ==, BIT_NO);
  g_assert_cmpstr (source, ==, "made in Norway");
  g_free (source);
  g_assert_cmpuint (gog_original_languages ("mystery", NULL), ==, 0);

  /* Out of the box: the original language only. */
  g_assert_true (gog_sync_get_default_original ());
  g_assert_cmpuint (gog_sync_get_default_languages (), ==, 0);
  guint offered = BIT_EN | BIT_DE | BIT_NO;
  g_assert_cmpuint (gog_sync_default_languages_for ("among_the_sleep", offered), ==, BIT_NO);
  /* Original not known: English, or all when there is no English. */
  g_assert_cmpuint (gog_sync_default_languages_for ("mystery", offered), ==, BIT_EN);
  g_assert_cmpuint (gog_sync_default_languages_for ("mystery", BIT_DE | BIT_FR), ==, 0);

  gog_sync_set_default_languages (BIT_EN);
  g_assert_cmpuint (gog_sync_default_languages_for ("among_the_sleep", offered), ==,
                    BIT_EN | BIT_NO);
  gog_sync_set_default_original (FALSE);
  g_assert_cmpuint (gog_sync_default_languages_for ("among_the_sleep", offered), ==, BIT_EN);
  gog_sync_set_default_languages (0);
  g_assert_cmpuint (gog_sync_default_languages_for ("among_the_sleep", offered), ==, 0);
  gog_sync_set_default_original (TRUE);

  /* Per game: nothing stored is the default; "all" and choices are kept. */
  GogSyncItem item = { .gamename = (gchar *) "among_the_sleep", .languages_offered = offered };
  g_assert_cmpuint (gog_sync_get_game_languages ("among_the_sleep"), ==,
                    GOG_SYNC_LANGUAGES_DEFAULT);
  g_assert_cmpuint (gog_sync_item_languages (&item), ==, BIT_NO);
  gog_sync_set_game_languages ("among_the_sleep", 0);
  g_assert_cmpuint (gog_sync_get_game_languages ("among_the_sleep"), ==, 0);
  g_assert_cmpuint (gog_sync_item_languages (&item), ==, 0);
  gog_sync_set_game_languages ("among_the_sleep", BIT_DE);
  g_assert_cmpuint (gog_sync_item_languages (&item), ==, BIT_DE);
  gog_sync_set_game_languages ("among_the_sleep", GOG_SYNC_LANGUAGES_DEFAULT);
  g_assert_cmpuint (gog_sync_item_languages (&item), ==, BIT_NO);
  /* Details missing: the choice still holds, it doesn't become "all". */
  GogSyncItem unknown = { .gamename = (gchar *) "among_the_sleep", .languages_offered = 0 };
  gog_sync_set_game_languages ("among_the_sleep", BIT_DE);
  g_assert_cmpuint (gog_sync_item_languages (&unknown), ==, BIT_DE);
  /* A chosen language the game no longer has: the default. */
  gog_sync_set_game_languages ("among_the_sleep", BIT_FR);
  g_assert_cmpuint (gog_sync_item_languages (&item), ==, BIT_NO);
  gog_sync_set_game_languages ("among_the_sleep", GOG_SYNC_LANGUAGES_DEFAULT);

  gchar *settings = g_build_filename (configs, "gog-manager-download.json", NULL);
  g_remove (settings);
  g_remove (cache);
  g_rmdir (configs);
  g_rmdir (tmp);
  g_unsetenv ("APPIMAGE");
  g_free (settings);
  g_free (cache);
  g_free (configs);
  g_free (fake);
  g_free (tmp);
}

/* Old versions are only deleted when a new file of the same kind replaced
 * them: the same name apart from version numbers. */
static void
test_version_blind_names (void)
{
  static const char *const same[][2] = {
    { "setup_postal_2_5100_(chinese_simplified)_(64474)-1.bin",
      "setup_postal_2_5200_(chinese_simplified)_(65001)-1.bin" },
    { "setup_fallout_4_1.10.163.0_(64bit)_(66700).exe", "setup_fallout_4_1.10.984.0_(64bit)_(70011).exe" },
    { "alone_in_the_dark_2_deDE_1_0_31909.pkg", "alone_in_the_dark_2_deDE_1_1_32000.pkg" },
  };
  static const char *const different[][2] = {
    { "setup_postal_2_5100_(chinese_simplified)_(64474)-1.bin", "setup_postal_2_5200_(65001)-1.bin" },
    { "alone_in_the_dark_2_deDE_1_0_31909.pkg", "alone_in_the_dark_2_enUS_1_0_31909.pkg" },
    { "setup_game_1.0_(100).exe", "game_1_0_100.pkg" },
  };
  for (guint i = 0; i < G_N_ELEMENTS (same); i++)
    {
      gchar *a = gog_sync_version_blind_name (same[i][0]);
      gchar *b = gog_sync_version_blind_name (same[i][1]);
      g_assert_cmpstr (a, ==, b);
      g_free (a);
      g_free (b);
    }
  for (guint i = 0; i < G_N_ELEMENTS (different); i++)
    {
      gchar *a = gog_sync_version_blind_name (different[i][0]);
      gchar *b = gog_sync_version_blind_name (different[i][1]);
      g_assert_cmpstr (a, !=, b);
      g_free (a);
      g_free (b);
    }
}

static GogGame *
installed_game (const char *installers, const char *install_path)
{
  GogGame *game = gog_game_new ();
  gog_game_set_folder_path (game, installers);
  gog_game_set_install_path (game, install_path);
  gog_game_set_installed (game, TRUE);
  return game;
}

/* Uninstall deletes the game folder, and never a folder that holds other
 * things: home, the installers' folder, a mount point. A symlinked game
 * folder is removed through its target. */
static void
test_uninstall_protects_folders (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-uninstall-XXXXXX", NULL);
  gchar *installers = g_build_filename (tmp, "installers", NULL);
  gchar *game_dir = g_build_filename (tmp, "games", "foo", NULL);
  gchar *game_file = g_build_filename (game_dir, "foo.exe", NULL);
  gchar *setup = g_build_filename (installers, "setup_foo.exe", NULL);
  g_assert_cmpint (g_mkdir_with_parents (installers, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (game_dir, 0755), ==, 0);
  g_assert_true (g_file_set_contents (game_file, "x", -1, NULL));
  g_assert_true (g_file_set_contents (setup, "x", -1, NULL));

  GError *error = NULL;
  /* Refused: home, the installers' folder, a folder holding it. */
  const char *refused[] = { g_get_home_dir (), installers, tmp, "/" };
  for (guint i = 0; i < G_N_ELEMENTS (refused); i++)
    {
      GogGame *game = installed_game (installers, refused[i]);
      g_assert_false (gog_uninstall_game (game, &error));
      g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_PERM);
      g_clear_error (&error);
      g_assert_true (gog_game_get_installed (game));
      g_object_unref (game);
    }
  g_assert_true (g_file_test (setup, G_FILE_TEST_EXISTS));

  /* A symlink to the game folder: the folder and the link go. */
  gchar *link = g_build_filename (tmp, "foo-link", NULL);
  g_assert_cmpint (symlink (game_dir, link), ==, 0);
  GogGame *game = installed_game (installers, link);
  g_assert_true (gog_uninstall_game (game, &error));
  g_assert_no_error (error);
  g_assert_false (gog_game_get_installed (game));
  g_assert_false (g_file_test (game_dir, G_FILE_TEST_EXISTS));
  g_assert_false (g_file_test (link, G_FILE_TEST_EXISTS) ||
                  g_file_test (link, G_FILE_TEST_IS_SYMLINK));
  g_assert_true (g_file_test (setup, G_FILE_TEST_EXISTS));
  g_object_unref (game);

  g_remove (setup);
  g_rmdir (installers);
  gchar *games = g_build_filename (tmp, "games", NULL);
  g_rmdir (games);
  g_rmdir (tmp);
  g_free (games);
  g_free (link);
  g_free (setup);
  g_free (game_file);
  g_free (game_dir);
  g_free (installers);
  g_free (tmp);
}

/* Files an older version kept next to the AppImage move into
 * .gog-manager-configs; one already there is not overwritten. */
static void
test_config_files_move_to_config_folder (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-configs-XXXXXX", NULL);
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE);
  gchar *old_state = g_build_filename (tmp, "gog-manager-state.json", NULL);
  gchar *old_cfg = g_build_filename (tmp, "gog-manager.cfg", NULL);
  gchar *configs = g_build_filename (tmp, ".gog-manager-configs", NULL);
  gchar *new_state = g_build_filename (configs, "gog-manager-state.json", NULL);
  gchar *new_cfg = g_build_filename (configs, "gog-manager.cfg", NULL);
  g_assert_true (g_file_set_contents (old_state, "{\"old\": 1}", -1, NULL));
  g_assert_true (g_file_set_contents (old_cfg, "{\"old\": 1}", -1, NULL));
  g_assert_cmpint (g_mkdir (configs, 0700), ==, 0);
  g_assert_true (g_file_set_contents (new_cfg, "{\"new\": 1}", -1, NULL));

  gchar *path = gog_config_app_file ("gog-manager-categories.json");
  gchar *expected = g_build_filename (configs, "gog-manager-categories.json", NULL);
  g_assert_cmpstr (path, ==, expected);

  g_assert_false (g_file_test (old_state, G_FILE_TEST_EXISTS));
  gchar *text = NULL;
  g_assert_true (g_file_get_contents (new_state, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "{\"old\": 1}");
  g_free (text);
  g_assert_true (g_file_test (old_cfg, G_FILE_TEST_EXISTS));
  g_assert_true (g_file_get_contents (new_cfg, &text, NULL, NULL));
  g_assert_cmpstr (text, ==, "{\"new\": 1}");
  g_free (text);

  g_remove (new_state);
  g_remove (new_cfg);
  g_remove (old_cfg);
  g_rmdir (configs);
  g_rmdir (tmp);
  g_unsetenv ("APPIMAGE");
  g_free (expected);
  g_free (path);
  g_free (new_cfg);
  g_free (new_state);
  g_free (configs);
  g_free (old_cfg);
  g_free (old_state);
  g_free (fake);
  g_free (tmp);
}

static void
remove_tree (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      gchar *child = g_build_filename (path, name, NULL);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        remove_tree (child);
      else
        g_remove (child);
      g_free (child);
    }
  if (d)
    g_dir_close (d);
  g_rmdir (path);
}

static GogLibrary *
scan (const char *root)
{
  GogLibrary *library = gog_library_new ();
  gboolean done = FALSE;
  gog_library_scan_async (library, root, NULL, on_scan_done, &done);
  while (!done)
    g_main_context_iteration (NULL, TRUE);
  while (g_main_context_iteration (NULL, FALSE))
    ;
  return library;
}

/* lgogdownloader's names from GOG's installer file names. */
static void
test_gamename_from_installer (void)
{
  static const char *const cases[][2] = {
    { "setup_beneath_a_steel_sky_1.0_(20270).exe", "beneath_a_steel_sky" },
    { "setup_akalabeth_world_of_doom_1.02_cs_(77607).exe", "akalabeth_world_of_doom" },
    { "setup_alone_in_the_dark_2_1.0_(28043).exe", "alone_in_the_dark_2" },
    { "setup_the_witcher_3_wild_hunt_v4.04a_redkit_(64bit)_(77663).exe", "the_witcher_3_wild_hunt" },
    { "setup_doom_64_(german)_(44337).exe", "doom_64" },
    { "setup_foo_20150.exe", "foo" },
    { "beneath_a_steel_sky_en_gog_2_20150.sh", "beneath_a_steel_sky" },
    { "gog_akalabeth_world_of_doom_2.0.0.3.sh", "akalabeth_world_of_doom" },
    { "stardew_valley_1_6_15_24356_8cc13b9d0a_76869.sh", "stardew_valley" },
    { "beneath_a_steel_sky_enUS_1_0_33348.pkg", "beneath_a_steel_sky" },
    { "setup_beneath_a_steel_sky_1.0_(20270)-1.bin", NULL },
    { "patch_doom_3_1.0_to_1.1.exe", NULL },
    { "Setup Something.exe", NULL },
    { "install.sh", NULL },
    { "setup_tool.exe", NULL },
    { "setup_tool_(1234).exe", "tool" },
    { "my_game.pkg", NULL },
    { "readme.txt", NULL },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      gchar *name = gog_library_gamename_from_installer (cases[i][0]);
      g_assert_cmpstr (name, ==, cases[i][1]);
      g_free (name);
    }
}

/* A folder of installers from GOG's website (no product file), in a folder
 * with a name of the user's own: found, named from the last GOG check, and
 * a folder that holds no GOG installer is left out. */
static void
test_scan_folder_without_product_file (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-installers-XXXXXX", NULL);
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE); /* the config folder goes in tmp */
  gchar *root = g_build_filename (tmp, "library", NULL);
  gchar *bass = g_build_filename (root, "Beneath a Steel Sky", NULL);
  gchar *other = g_build_filename (root, "Old Tools", NULL);
  gchar *unknown = g_build_filename (root, "Some Game", NULL);
  g_assert_cmpint (g_mkdir_with_parents (bass, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (other, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (unknown, 0755), ==, 0);
  write_file (bass, "setup_beneath_a_steel_sky_1.0_(20270).exe", "x");
  write_file (bass, "beneath_a_steel_sky_en_gog_2_20150.sh", "x");
  write_file (other, "Setup Tool.exe", "x");
  write_file (other, "notes.txt", "x");
  write_file (unknown, "setup_some_game_2.1_(1234).exe", "x");

  /* The last check listed both as not downloaded; it knows one's id. */
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);
  GogSyncItem *item = g_new0 (GogSyncItem, 1);
  item->gamename = g_strdup ("beneath_a_steel_sky");
  item->kind = GOG_SYNC_NEW_GAME;
  item->files = g_ptr_array_new ();
  item->title = g_strdup ("Beneath a Steel Sky (1994)");
  item->product_id = 1207658695;
  g_ptr_array_add (items, item);
  gog_sync_save_results (items);

  GogLibrary *library = scan (root);
  g_assert_cmpuint (g_list_model_get_n_items (gog_library_get_model (library)), ==, 2);
  GogGame *game = gog_library_find_by_id (library, 1207658695);
  g_assert_nonnull (game);
  g_assert_cmpstr (gog_game_get_title (game), ==, "Beneath a Steel Sky (1994)");
  g_assert_cmpstr (gog_game_get_gamename (game), ==, "beneath_a_steel_sky");
  g_assert_cmpuint (gog_game_get_installers (game)->len, ==, 2);
  g_assert_true (gog_game_get_supports_os (game, GOG_OS_WINDOWS));
  g_assert_true (gog_game_get_supports_os (game, GOG_OS_LINUX));
  g_assert_false (gog_game_get_supports_os (game, GOG_OS_MAC));
  g_assert_true (gog_game_get_downloaded (game));
  /* Not listed again as a game to download. */
  gog_library_set_not_downloaded (library, items);
  g_assert_cmpuint (g_list_model_get_n_items (gog_library_get_model (library)), ==, 2);

  /* Not in the check: a readable name and a stable local id. */
  GogGame *some = gog_library_find_by_id (library, gog_game_local_id ("some_game"));
  g_assert_nonnull (some);
  g_assert_cmpint (gog_game_get_id (some), <, 0);
  g_assert_cmpstr (gog_game_get_title (some), ==, "Some Game");

  g_object_unref (library);
  g_ptr_array_unref (items);
  g_unsetenv ("APPIMAGE");
  remove_tree (tmp);
  g_free (unknown);
  g_free (other);
  g_free (bass);
  g_free (root);
  g_free (fake);
  g_free (tmp);
}

/* An install made under the local id moves to GOG's id once it is known. */
static void
test_install_record_follows_gog_id (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-state-XXXXXX", NULL);
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE);

  GogGame *before = gog_game_new ();
  gog_game_set_gamename (before, "some_game");
  gog_game_set_id (before, gog_game_local_id ("some_game"));
  gog_game_set_installed (before, TRUE);
  gog_game_set_install_path (before, "/games/some_game");
  GogState *state = gog_state_load ();
  gog_state_update_from_game (state, before);
  g_assert_true (gog_state_save (state, NULL));
  gog_state_free (state);

  GogGame *after = gog_game_new ();
  gog_game_set_gamename (after, "some_game");
  gog_game_set_id (after, 42);
  state = gog_state_load ();
  gog_state_apply_to_game (state, after);
  g_assert_true (gog_game_get_installed (after));
  g_assert_cmpstr (gog_game_get_install_path (after), ==, "/games/some_game");
  /* And it is kept under the new id. */
  g_assert_true (gog_state_save (state, NULL));
  gog_state_free (state);
  GogGame *again = gog_game_new ();
  gog_game_set_id (again, 42);
  state = gog_state_load ();
  gog_state_apply_to_game (state, again);
  g_assert_true (gog_game_get_installed (again));
  gog_state_free (state);

  g_object_unref (before);
  g_object_unref (after);
  g_object_unref (again);
  g_unsetenv ("APPIMAGE");
  remove_tree (tmp);
  g_free (fake);
  g_free (tmp);
}

/* Play history is kept apart from the install record: it survives an
 * uninstall, adds up, and moves to GOG's id like the record does. */
static void
test_play_history_kept (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-state-XXXXXX", NULL);
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE);

  GogGame *game = gog_game_new ();
  gog_game_set_gamename (game, "some_game");
  gog_game_set_id (game, gog_game_local_id ("some_game"));
  GogState *state = gog_state_load ();
  gog_state_record_play (state, game, 1000, 0);
  gog_state_record_play (state, game, 1000, 600);
  gog_state_record_play (state, game, 5000, 60);
  /* An older session ending late doesn't make it older. */
  gog_state_record_play (state, game, 2000, 30);
  g_assert_cmpint (gog_game_get_last_played (game), ==, 5000);
  g_assert_cmpint (gog_game_get_play_seconds (game), ==, 690);
  gog_state_update_from_game (state, game);
  gog_state_remove (state, gog_game_get_id (game));
  g_assert_true (gog_state_save (state, NULL));
  gog_state_free (state);

  GogGame *later = gog_game_new ();
  gog_game_set_gamename (later, "some_game");
  gog_game_set_id (later, 42);
  state = gog_state_load ();
  gog_state_apply_to_game (state, later);
  g_assert_cmpint (gog_game_get_last_played (later), ==, 5000);
  g_assert_cmpint (gog_game_get_play_seconds (later), ==, 690);
  g_assert_false (gog_game_get_installed (later));
  gog_state_free (state);

  g_object_unref (game);
  g_object_unref (later);
  g_unsetenv ("APPIMAGE");
  remove_tree (tmp);
  g_free (fake);
  g_free (tmp);
}

/* A renamed game folder is noticed, and renaming it back moves the game's
 * paths (and its DLC's) along. A folder of that name already there stops it. */
static void
test_rename_misnamed_folder (void)
{
  gchar *tmp = g_dir_make_tmp ("gog-rename-XXXXXX", NULL);
  gchar *fake = g_build_filename (tmp, "gog-manager.AppImage", NULL);
  g_setenv ("APPIMAGE", fake, TRUE);
  gchar *root = g_build_filename (tmp, "library", NULL);
  gchar *renamed = g_build_filename (root, "My Favourite Game", NULL);
  gchar *dlc = g_build_filename (renamed, "dlc", "fav_dlc", NULL);
  gchar *right = g_build_filename (root, "fav_game", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dlc, 0755), ==, 0);
  write_file (renamed, "product_fav_game.json", "{\"id\": 7, \"title\": \"Fav\"}");
  write_file (renamed, "setup_fav_game_1.0_(1).exe", "x");
  write_file (dlc, "product_fav_dlc.json",
              "{\"id\": 8, \"title\": \"Fav DLC\", \"game_type\": \"dlc\"}");

  GogLibrary *library = scan (root);
  GogGame *game = gog_library_find_by_id (library, 7);
  g_assert_nonnull (game);
  g_assert_cmpstr (gog_game_get_gamename (game), ==, "fav_game");
  g_assert_true (gog_library_folder_misnamed (game, root));
  g_assert_true (gog_library_find_by_gamename (library, "fav_game") == game);

  /* Blocked by a folder already named so. */
  g_assert_cmpint (g_mkdir (right, 0755), ==, 0);
  GError *error = NULL;
  g_assert_false (gog_library_rename_folder (library, game, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_clear_error (&error);
  g_rmdir (right);

  g_assert_true (gog_library_rename_folder (library, game, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (gog_game_get_folder_path (game), ==, right);
  g_assert_false (gog_library_folder_misnamed (game, root));
  GogInstallerInfo *installer = g_ptr_array_index (gog_game_get_installers (game), 0);
  gchar *expected = g_build_filename (right, "setup_fav_game_1.0_(1).exe", NULL);
  g_assert_cmpstr (installer->file_path, ==, expected);
  g_assert_true (g_file_test (expected, G_FILE_TEST_IS_REGULAR));
  g_free (expected);
  GogGame *dlc_game = gog_library_find_by_id (library, 8);
  expected = g_build_filename (right, "dlc", "fav_dlc", NULL);
  g_assert_cmpstr (gog_game_get_folder_path (dlc_game), ==, expected);
  g_free (expected);

  g_object_unref (library);
  g_unsetenv ("APPIMAGE");
  remove_tree (tmp);
  g_free (right);
  g_free (dlc);
  g_free (renamed);
  g_free (root);
  g_free (fake);
  g_free (tmp);
}

/* The estimate, and free space for an install folder that doesn't exist
 * yet: the nearest folder above it is asked. */
static void
test_install_space (void)
{
  g_assert_cmpuint (gog_install_estimate_bytes (1000, FALSE), ==, 2000);
  g_assert_cmpuint (gog_install_estimate_bytes (1000, TRUE), ==, 2000 + (1u << 30));
  gchar *tmp = g_dir_make_tmp ("gog-space-XXXXXX", NULL);
  gchar *missing = g_build_filename (tmp, "not", "there", "yet", NULL);
  guint64 free_bytes = 0;
  g_assert_true (gog_free_space (missing, &free_bytes));
  g_assert_cmpuint (free_bytes, >, 0);
  g_assert_false (gog_free_space ("relative/path", &free_bytes));
  g_rmdir (tmp);
  g_free (missing);
  g_free (tmp);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/library/scan_reads_serials", test_scan_reads_serials);
  g_test_add_func ("/library/scan_reads_product_info", test_scan_reads_product_info);
  g_test_add_func ("/library/language_from_installer_name", test_language_from_installer_name);
  g_test_add_func ("/library/scan_detects_installer_languages", test_scan_detects_installer_languages);
  g_test_add_func ("/library/all_languages_listed_once", test_all_languages_listed_once);
  g_test_add_func ("/gog_sync/parse_status", test_gog_sync_parse_status);
  g_test_add_func ("/gog_sync/default_download_languages", test_default_download_languages);
  g_test_add_func ("/gog_sync/version_blind_names", test_version_blind_names);
  g_test_add_func ("/install/uninstall_protects_folders", test_uninstall_protects_folders);
  g_test_add_func ("/config/files_move_to_config_folder", test_config_files_move_to_config_folder);
  g_test_add_func ("/install/space", test_install_space);
  g_test_add_func ("/library/gamename_from_installer", test_gamename_from_installer);
  g_test_add_func ("/library/scan_folder_without_product_file",
                   test_scan_folder_without_product_file);
  g_test_add_func ("/config/install_record_follows_gog_id", test_install_record_follows_gog_id);
  g_test_add_func ("/config/play_history_kept", test_play_history_kept);
  g_test_add_func ("/library/rename_misnamed_folder", test_rename_misnamed_folder);
  return g_test_run ();
}
