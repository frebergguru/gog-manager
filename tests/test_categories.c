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

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/categories/parse-order", test_parse_keeps_gog_order);
  g_test_add_func ("/categories/parse-no-tags", test_parse_without_tags_is_empty);
  g_test_add_func ("/categories/parse-garbage", test_parse_rejects_garbage);
  return g_test_run ();
}
