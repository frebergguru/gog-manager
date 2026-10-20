/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include "../src/categories.h"

static void
test_parse_keeps_gog_order (void)
{
  GStrv cats = gog_categories_parse (
      "{ \"id\": 1, \"_embedded\": { \"product\": {}, \"tags\": ["
      "  { \"id\": \"1\", \"name\": \"Role-playing\" },"
      "  { \"id\": \"2\", \"name\": \"Action\" },"
      "  { \"id\": \"2\", \"name\": \"Action\" },"
      "  { \"id\": \"3\", \"name\": \"\" },"
      "  { \"id\": \"4\", \"name\": \"Fantasy\" } ] } }");
  g_assert_nonnull (cats);
  g_assert_cmpuint (g_strv_length (cats), ==, 3);
  g_assert_cmpstr (cats[0], ==, "Role-playing");
  g_assert_cmpstr (cats[1], ==, "Action");
  g_assert_cmpstr (cats[2], ==, "Fantasy");
  g_strfreev (cats);
}

static void
test_parse_without_tags_is_empty (void)
{
  GStrv cats = gog_categories_parse ("{ \"_embedded\": { \"product\": {} } }");
  g_assert_nonnull (cats);
  g_assert_null (cats[0]);
  g_strfreev (cats);
}

static void
test_parse_rejects_garbage (void)
{
  g_assert_null (gog_categories_parse ("<html>busy</html>"));
  g_assert_null (gog_categories_parse ("[1, 2]"));
  g_assert_null (gog_categories_parse (NULL));
}

static void
test_store_info_parse (void)
{
  g_autoptr (GogStoreInfo) info = gog_store_info_parse (
      "{ \"copyrights\": \"<p>\u00a9 2016 Bethesda</p>\", \"isUsingDosBox\": true,"
      "  \"inDevelopment\": { \"active\": true },"
      "  \"_embedded\": {"
      "    \"features\": [ { \"name\": \"Cloud saves\" }, { \"name\": \"Cloud saves\" } ],"
      "    \"properties\": [ { \"name\": \"Sci-fi\" } ],"
      "    \"localizations\": ["
      "      { \"_embedded\": { \"language\": { \"name\": \"German\" },"
      "                         \"localizationScope\": { \"type\": \"audio\" } } },"
      "      { \"_embedded\": { \"language\": { \"name\": \"English\" },"
      "                         \"localizationScope\": { \"type\": \"text\" } } } ],"
      "    \"esrbRating\": { \"category\": { \"name\": \"Mature 17+\" }, \"ageRating\": 17 },"
      "    \"pegiRating\": { \"ageRating\": 18, \"contentDescriptors\": [ { \"descriptor\": \"Violence\\n\" } ] },"
      "    \"editions\": [ { \"name\": \"Standard\" } ],"
      "    \"supportedOperatingSystems\": [ { \"operatingSystem\": { \"name\": \"osx\" },"
      "      \"systemRequirements\": ["
      "        { \"type\": \"minimum\", \"requirements\": [ { \"name\": \"Memory:\", \"description\": \"4 GB\" },"
      "                                                     { \"name\": \"Sound:\", \"description\": \"\" } ] },"
      "        { \"type\": \"recommended\", \"description\": \"Fast<br>computer\", \"requirements\": [] } ] } ] } }");
  g_assert_nonnull (info);
  g_assert_cmpuint (g_strv_length (info->features), ==, 1);
  g_assert_cmpstr (info->tags[0], ==, "Sci-fi");
  g_assert_cmpstr (info->audio_languages[0], ==, "German");
  g_assert_null (info->audio_languages[1]);
  g_assert_cmpstr (info->text_languages[0], ==, "English");
  g_assert_cmpstr (info->age_ratings[0], ==, "ESRB Mature 17+");
  g_assert_cmpstr (info->age_ratings[1], ==, "PEGI 18 (Violence)");
  /* One edition is just the game. */
  g_assert_null (info->editions[0]);
  g_assert_cmpstr (info->technologies[0], ==, "DOSBox");
  g_assert_cmpstr (info->copyright, ==, "\xC2\xA9 2016 Bethesda");
  g_assert_true (info->in_development);
  g_assert_cmpuint (info->requirements->len, ==, 2);
  GogRequirements *min = g_ptr_array_index (info->requirements, 0);
  g_assert_cmpstr (min->os, ==, "macOS");
  g_assert_cmpstr (min->level, ==, "Minimum");
  g_assert_cmpstr (min->names[0], ==, "Memory");
  g_assert_cmpstr (min->values[0], ==, "4 GB");
  /* An empty part is left out. */
  g_assert_null (min->names[1]);
  GogRequirements *rec = g_ptr_array_index (info->requirements, 1);
  g_assert_cmpstr (rec->level, ==, "Recommended");
  g_assert_cmpstr (rec->values[0], ==, "Fast\ncomputer");

  g_autoptr (GogStoreInfo) same = gog_store_info_parse ("{ \"_embedded\": {} }");
  g_assert_false (gog_store_info_equal (info, same));
  g_assert_null (gog_store_info_parse ("{ \"error\": 1 }"));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/categories/parse-order", test_parse_keeps_gog_order);
  g_test_add_func ("/categories/parse-no-tags", test_parse_without_tags_is_empty);
  g_test_add_func ("/categories/parse-garbage", test_parse_rejects_garbage);
  g_test_add_func ("/categories/store-info", test_store_info_parse);
  return g_test_run ();
}
