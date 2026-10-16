/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <glib.h>
#include <string.h>
#include "../src/json_util.h"
#include "../src/text_util.h"

static void
test_languages_dict (void)
{
  json_object *root = json_tokener_parse ("{\"languages\": {\"en\":\"English\",\"fr\":\"français\"}}");
  GStrv langs = ju_get_languages (root);
  g_assert_nonnull (langs);
  guint n = g_strv_length (langs);
  g_assert_cmpuint (n, ==, 2);
  g_assert_true (g_strv_contains ((const char *const *) langs, "English"));
  g_assert_true (g_strv_contains ((const char *const *) langs, "français"));
  g_strfreev (langs);
  json_object_put (root);
}

static void
test_languages_array (void)
{
  json_object *root = json_tokener_parse ("{\"languages\": [\"en\", \"de\"]}");
  GStrv langs = ju_get_languages (root);
  g_assert_cmpuint (g_strv_length (langs), ==, 2);
  g_assert_true (g_strv_contains ((const char *const *) langs, "en"));
  g_assert_true (g_strv_contains ((const char *const *) langs, "de"));
  g_strfreev (langs);
  json_object_put (root);
}

static void
test_languages_missing (void)
{
  json_object *root = json_tokener_parse ("{}");
  GStrv langs = ju_get_languages (root);
  g_assert_nonnull (langs);
  g_assert_cmpuint (g_strv_length (langs), ==, 0);
  g_strfreev (langs);
  json_object_put (root);
}

static void
test_get_string_missing_field_tolerance (void)
{
  json_object *root = json_tokener_parse ("{\"a\": 1}");
  const char *s = ju_get_string (root, "missing", "default");
  g_assert_cmpstr (s, ==, "default");
  const char *wrong_type = ju_get_string (root, "a", "default");
  g_assert_cmpstr (wrong_type, ==, "default"); /* "a" is an int, not a string */
  json_object_put (root);
}

static void
test_strip_html_basic (void)
{
  gchar *out = text_strip_html ("Hello<br>World &amp; friends");
  g_assert_cmpstr (out, ==, "Hello\nWorld & friends");
  g_free (out);
}

static void
test_strip_html_list (void)
{
  gchar *out = text_strip_html ("<ul><li>One</li><li>Two</li></ul>");
  g_assert_nonnull (strstr (out, "\xE2\x80\xA2 One"));
  g_assert_nonnull (strstr (out, "\xE2\x80\xA2 Two"));
  g_free (out);
}

static void
test_strip_html_empty (void)
{
  gchar *out = text_strip_html (NULL);
  g_assert_cmpstr (out, ==, "");
  g_free (out);
  out = text_strip_html ("");
  g_assert_cmpstr (out, ==, "");
  g_free (out);
}

static void
test_strip_html_numeric_entities (void)
{
  gchar *out = text_strip_html ("It&#8217;s &#x41;&#66; &#xZZ; &#0;");
  g_assert_cmpstr (out, ==, "It\xE2\x80\x99s AB &#xZZ; &#0;");
  g_free (out);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/json_util/languages_dict", test_languages_dict);
  g_test_add_func ("/json_util/languages_array", test_languages_array);
  g_test_add_func ("/json_util/languages_missing", test_languages_missing);
  g_test_add_func ("/json_util/get_string_tolerance", test_get_string_missing_field_tolerance);
  g_test_add_func ("/text_util/strip_html_basic", test_strip_html_basic);
  g_test_add_func ("/text_util/strip_html_list", test_strip_html_list);
  g_test_add_func ("/text_util/strip_html_empty", test_strip_html_empty);
  g_test_add_func ("/text_util/strip_html_numeric_entities", test_strip_html_numeric_entities);
  return g_test_run ();
}
