/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include "../src/gamesdb.h"

static void
test_parse_reads_details (void)
{
  g_autoptr (GogGamesdbInfo) info = gog_gamesdb_parse (
      "{ \"external_id\": \"1\","
      "  \"summary\": { \"*\": \"About GOG's version. \", \"en-US\": \"\" },"
      "  \"series\": null,"
      "  \"game\": {"
      "    \"summary\": { \"*\": \"General.\" },"
      "    \"series\": { \"name\": \"The Witcher\" },"
      "    \"developers\": [ { \"name\": \"CD Projekt RED\" }, { \"name\": \"CD Projekt RED\" } ],"
      "    \"publishers\": [ { \"name\": \"CD Projekt\" } ],"
      "    \"themes\": [ { \"name\": { \"*\": \"Open world\", \"en-US\": \"Open World\" } } ],"
      "    \"game_modes\": [ { \"name\": \"Single player\" }, { \"name\": \"\" } ],"
      "    \"vertical_cover\": { \"url_format\": "
      "      \"https://images.gog.com/abc{formatter}.{ext}?namespace=gamesdb\" },"
      "    \"background\": { \"url_format\": \"http://example.com/bg{formatter}.{ext}\" } } }");
  g_assert_nonnull (info);
  /* The release's summary first; an empty English one falls back to "*". */
  g_assert_cmpstr (info->summary, ==, "About GOG's version.");
  g_assert_cmpstr (info->series, ==, "The Witcher");
  g_assert_cmpuint (g_strv_length (info->developers), ==, 1);
  g_assert_cmpstr (info->developers[0], ==, "CD Projekt RED");
  g_assert_cmpstr (info->publishers[0], ==, "CD Projekt");
  g_assert_cmpstr (info->themes[0], ==, "Open World");
  g_assert_cmpuint (g_strv_length (info->game_modes), ==, 1);
  g_assert_cmpstr (info->cover_url, ==,
                   "https://images.gog.com/abc_glx_vertical_cover.jpg?namespace=gamesdb");
  /* Only https. */
  g_assert_null (info->background_url);
  g_assert_null (info->cover_path);
}

static void
test_parse_minimal (void)
{
  g_autoptr (GogGamesdbInfo) info = gog_gamesdb_parse ("{ \"game\": {} }");
  g_assert_nonnull (info);
  g_assert_null (info->summary);
  g_assert_nonnull (info->themes);
  g_assert_null (info->themes[0]);
  g_assert_null (info->cover_url);
}

static void
test_parse_rejects_garbage (void)
{
  g_assert_null (gog_gamesdb_parse ("<html>busy</html>"));
  g_assert_null (gog_gamesdb_parse ("[1, 2]"));
  g_assert_null (gog_gamesdb_parse ("{ \"error\": \"not found\" }"));
  g_assert_null (gog_gamesdb_parse (NULL));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gamesdb/parse-details", test_parse_reads_details);
  g_test_add_func ("/gamesdb/parse-minimal", test_parse_minimal);
  g_test_add_func ("/gamesdb/parse-garbage", test_parse_rejects_garbage);
  return g_test_run ();
}
