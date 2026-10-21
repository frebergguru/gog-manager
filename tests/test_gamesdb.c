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
test_parse_reads_media (void)
{
  g_autoptr (GogGamesdbInfo) info = gog_gamesdb_parse (
      "{ \"videos\": [ { \"provider\": \"youtube\", \"video_id\": \"PZ_gex3U31U\","
      "                  \"name\": null } ],"
      "  \"game\": {"
      "    \"first_release_date\": \"2008-09-16T00:00:00+0000\","
      "    \"aggregated_rating\": 85.5,"
      "    \"genres\": [ { \"name\": { \"*\": \"Role-playing (RPG)\" } } ],"
      "    \"screenshots\": [ { \"url_format\": \"https://images.gog.com/a{formatter}.{ext}\" },"
      "                       { \"url_format\": \"https://images.gog.com/a{formatter}.{ext}\" },"
      "                       { \"url_format\": \"http://images.gog.com/b{formatter}.{ext}\" } ],"
      "    \"artworks\": [ { \"url_format\": \"https://images.gog.com/c{formatter}.{ext}\" } ],"
      "    \"videos\": ["
      "      { \"provider\": \"youtube\", \"video_id\": \"PZ_gex3U31U\", \"name\": \"Trailer\" },"
      "      { \"provider\": \"youtube\", \"video_id\": \"vMgX5QNjQOA\", \"name\": \"Gameplay\" },"
      "      { \"provider\": \"youtube\", \"video_id\": \"x&y=1\", \"name\": \"Bad\" },"
      "      { \"provider\": \"vimeo\", \"video_id\": \"123\", \"name\": \"Elsewhere\" } ] } }");
  g_assert_nonnull (info);
  g_assert_cmpstr (info->genres[0], ==, "Role-playing (RPG)");
  g_assert_cmpint (info->first_release, ==, 1221523200);
  g_assert_cmpfloat (info->rating, ==, 85.5);
  /* Screenshots then artwork, https only, no repeats. */
  g_assert_cmpuint (g_strv_length (info->screenshots), ==, 2);
  g_assert_cmpstr (info->screenshots[1], ==, "https://images.gog.com/c{formatter}.{ext}");
  /* The release's videos first; a repeat is dropped, and so are odd ids
   * and other sites. */
  g_assert_cmpuint (g_strv_length (info->video_ids), ==, 2);
  g_assert_cmpstr (info->video_ids[0], ==, "PZ_gex3U31U");
  g_assert_cmpstr (info->video_names[0], ==, "");
  g_assert_cmpstr (info->video_ids[1], ==, "vMgX5QNjQOA");
  g_assert_cmpstr (info->video_names[1], ==, "Gameplay");

  g_autofree gchar *thumb = gog_gamesdb_screenshot_url (info->screenshots[0], FALSE);
  g_autofree gchar *full = gog_gamesdb_screenshot_url (info->screenshots[0], TRUE);
  g_assert_cmpstr (thumb, ==, "https://images.gog.com/a_ggvgm.jpg");
  g_assert_cmpstr (full, ==, "https://images.gog.com/a_1600.jpg");
  g_assert_null (gog_gamesdb_screenshot_url ("http://example.com/{formatter}.{ext}", FALSE));
}

static void
test_info_equal (void)
{
  const char *json = "{ \"game\": { \"genres\": [ { \"name\": \"Strategy\" } ] } }";
  g_autoptr (GogGamesdbInfo) a = gog_gamesdb_parse (json);
  g_autoptr (GogGamesdbInfo) b = gog_gamesdb_parse (json);
  g_assert_true (gog_gamesdb_info_equal (a, b));
  g_assert_true (gog_gamesdb_info_equal (NULL, NULL));
  g_assert_false (gog_gamesdb_info_equal (a, NULL));
  b->rating = 70;
  g_assert_false (gog_gamesdb_info_equal (a, b));
}

/* The Witcher 3's summary is HTML. */
static void
test_parse_html_summary (void)
{
  g_autoptr (GogGamesdbInfo) info = gog_gamesdb_parse (
      "{ \"game\": { \"summary\": { \"*\": \"<p>Become a <b>monster slayer</b>.</p>\" } } }");
  g_assert_nonnull (info);
  g_assert_cmpstr (info->summary, ==, "Become a monster slayer.");
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
  g_assert_nonnull (info->screenshots);
  g_assert_null (info->screenshots[0]);
  g_assert_null (info->video_ids[0]);
  g_assert_cmpint (info->first_release, ==, 0);
  g_assert_cmpfloat (info->rating, ==, 0);
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
  g_test_add_func ("/gamesdb/parse-media", test_parse_reads_media);
  g_test_add_func ("/gamesdb/info-equal", test_info_equal);
  g_test_add_func ("/gamesdb/parse-minimal", test_parse_minimal);
  g_test_add_func ("/gamesdb/parse-html-summary", test_parse_html_summary);
  g_test_add_func ("/gamesdb/parse-garbage", test_parse_rejects_garbage);
  return g_test_run ();
}
