/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "../src/cloud_saves.h"

static void
test_builds (void)
{
  /* The default branch's newest Gen 2 build, before a beta's. */
  gchar *link = gog_cloud_parse_builds (
      "{\"items\": ["
      " {\"generation\": 1, \"branch\": null, \"link\": \"https://x/gen1\"},"
      " {\"generation\": 2, \"branch\": \"compatibility\", \"link\": \"https://x/compat\"},"
      " {\"generation\": 2, \"branch\": null, \"link\": \"https://x/main\"}]}");
  g_assert_cmpstr (link, ==, "https://x/main");
  g_free (link);
  link = gog_cloud_parse_builds ("{\"items\": [{\"generation\": 2, \"branch\": \"beta\", \"link\": \"https://x/b\"}]}");
  g_assert_cmpstr (link, ==, "https://x/b");
  g_free (link);
  g_assert_null (gog_cloud_parse_builds ("{\"items\": []}"));
  g_assert_null (gog_cloud_parse_builds ("not json"));
}

static void
test_manifest (void)
{
  const char *json = "{\"clientId\": \"123\", \"clientSecret\": \"abc\", \"installDirectory\": \"G\"}";
  gchar *id = NULL, *secret = NULL;
  g_assert_true (gog_cloud_parse_manifest ((const guint8 *) json, strlen (json), &id, &secret));
  g_assert_cmpstr (id, ==, "123");
  g_assert_cmpstr (secret, ==, "abc");
  g_free (id);
  g_free (secret);

  /* As GOG's CDN gives it: zlib-compressed. */
  GZlibCompressor *z = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB, -1);
  guint8 packed[512];
  gsize read = 0, written = 0;
  g_assert_cmpint (g_converter_convert (G_CONVERTER (z), json, strlen (json), packed, sizeof packed,
                                        G_CONVERTER_INPUT_AT_END, &read, &written, NULL),
                   ==, G_CONVERTER_FINISHED);
  g_object_unref (z);
  g_assert_true (gog_cloud_parse_manifest (packed, written, &id, &secret));
  g_assert_cmpstr (id, ==, "123");
  g_free (id);
  g_free (secret);

  g_assert_false (gog_cloud_parse_manifest ((const guint8 *) "{}", 2, &id, &secret));
  g_assert_null (id);
}

static void
test_locations (void)
{
  GStrv l = gog_cloud_parse_locations (
      "{\"content\": {\"MacOS\": {\"cloudStorage\": {\"enabled\": true}},"
      " \"Windows\": {\"cloudStorage\": {\"enabled\": true, \"locations\": ["
      "  {\"name\": \"saves\", \"location\": \"<?APPLICATION_DATA_ROAMING?>\\\\\\\\StardewValley\"},"
      "  {\"name\": \"../evil\", \"location\": \"<?INSTALL?>\"}]}}}}",
      "42");
  g_assert_cmpuint (g_strv_length (l), ==, 2);
  g_assert_cmpstr (l[0], ==, "saves");
  g_assert_cmpstr (l[1], ==, "<?APPLICATION_DATA_ROAMING?>\\\\StardewValley");
  g_strfreev (l);

  /* On, but no places named: Galaxy's own. */
  l = gog_cloud_parse_locations ("{\"content\": {\"Windows\": {\"cloudStorage\": {\"enabled\": true, \"locations\": []}}}}", "42");
  g_assert_cmpstr (l[0], ==, "__default");
  g_assert_cmpstr (l[1], ==, "<?APPLICATION_DATA_LOCAL?>/GOG.com/Galaxy/Applications/42/Storage");
  g_strfreev (l);

  g_assert_null (gog_cloud_parse_locations ("{\"content\": {\"Windows\": {\"cloudStorage\": {\"enabled\": false}}}}", "42"));
  g_assert_null (gog_cloud_parse_locations ("{\"content\": {}}", "42"));
}

static void
test_resolve (void)
{
  gchar *root = g_dir_make_tmp ("gog-cloud-XXXXXX", NULL);
  gchar *pfx = g_build_filename (root, "pfx", NULL);
  /* Wine named the user folder after another login, and the game wrote
   * "MyGame" where GOG says "mygame". */
  gchar *roaming = g_build_filename (pfx, "drive_c", "users", "someone", "AppData", "Roaming",
                                     "MyGame", NULL);
  g_assert_cmpint (g_mkdir_with_parents (roaming, 0755), ==, 0);
  GogCloudGame game = { 1, "/games/g", pfx, "steamuser" };

  gchar *path = gog_cloud_resolve_location ("<?APPLICATION_DATA_ROAMING?>\\\\mygame\\saves", &game);
  gchar *want = g_build_filename (roaming, "saves", NULL);
  g_assert_cmpstr (path, ==, want);
  g_free (want);
  g_free (path);

  path = gog_cloud_resolve_location ("<?INSTALL?>/Saves", &game);
  g_assert_cmpstr (path, ==, "/games/g/Saves");
  g_free (path);

  path = gog_cloud_resolve_location ("<?SAVED_GAMES?>/G", &game);
  want = g_build_filename (pfx, "drive_c", "users", "someone", "Saved Games", "G", NULL);
  g_assert_cmpstr (path, ==, want);
  g_free (want);
  g_free (path);

  path = gog_cloud_resolve_location ("C:\\ProgramData\\G", &game);
  want = g_build_filename (pfx, "drive_c", "ProgramData", "G", NULL);
  g_assert_cmpstr (path, ==, want);
  g_free (want);
  g_free (path);

  g_assert_null (gog_cloud_resolve_location ("<?HOME?>/.config/G", &game));
  g_assert_null (gog_cloud_resolve_location ("D:\\G", &game));

  gchar *argv[] = { (gchar *) "rm", (gchar *) "-rf", root, NULL };
  g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free (roaming);
  g_free (pfx);
  g_free (root);
}

static void
test_listing (void)
{
  GPtrArray *files = gog_cloud_parse_listing (
      "[{\"name\": \"saves/b.sav\", \"bytes\": 10, \"hash\": \"ABCDEF\","
      "  \"last_modified\": \"2026-10-01T12:00:00+00:00\"},"
      " {\"name\": \"saves/a.sav\", \"bytes\": 5, \"last_modified\": \"2026-10-01T12:00:00\"},"
      " {\"name\": \"saves/../../etc/x\", \"bytes\": 1},"
      " {\"name\": \"/abs\", \"bytes\": 1},"
      " {\"name\": \"nofolder\", \"bytes\": 1}]");
  g_assert_cmpuint (files->len, ==, 2);
  GogCloudFile *a = g_ptr_array_index (files, 0);
  GogCloudFile *b = g_ptr_array_index (files, 1);
  g_assert_cmpstr (a->name, ==, "saves/a.sav");
  g_assert_cmpint (a->cloud_mtime, ==, 1790856000);
  g_assert_null (a->cloud_hash);
  g_assert_cmpstr (b->cloud_hash, ==, "abcdef");
  g_assert_cmpuint (b->cloud_size, ==, 10);
  g_assert_cmpint (b->state, ==, GOG_CLOUD_ONLY_ON_GOG);
  g_ptr_array_unref (files);

  g_assert_true (gog_cloud_name_is_safe ("saves/slot 1/data.bin"));
  g_assert_false (gog_cloud_name_is_safe ("saves//x"));
  g_assert_false (gog_cloud_name_is_safe ("saves/./x"));
  g_assert_false (gog_cloud_name_is_safe ("saves\\x"));
  g_assert_false (gog_cloud_name_is_safe (""));
}

static void
test_compare (void)
{
  /* The same content is the same, whatever the times. */
  g_assert_cmpint (gog_cloud_compare (100, 5, "aa", 5000, 5, "AA"), ==, GOG_CLOUD_IN_SYNC);
  g_assert_cmpint (gog_cloud_compare (5000, 5, "aa", 100, 5, "bb"), ==, GOG_CLOUD_NEWER_HERE);
  g_assert_cmpint (gog_cloud_compare (100, 5, "aa", 5000, 5, "bb"), ==, GOG_CLOUD_NEWER_ON_GOG);
  /* GOG's time, given to a file fetched or sent: the same, within a
   * second or two. */
  g_assert_cmpint (gog_cloud_compare (1001, 5, NULL, 1000, 5, NULL), ==, GOG_CLOUD_IN_SYNC);
  g_assert_cmpint (gog_cloud_compare (1000, 7, "aa", 1000, 5, "bb"), ==, GOG_CLOUD_NEWER_HERE);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cloud/builds", test_builds);
  g_test_add_func ("/cloud/manifest", test_manifest);
  g_test_add_func ("/cloud/locations", test_locations);
  g_test_add_func ("/cloud/resolve", test_resolve);
  g_test_add_func ("/cloud/listing", test_listing);
  g_test_add_func ("/cloud/compare", test_compare);
  return g_test_run ();
}
