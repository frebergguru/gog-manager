/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "text_util.h"
#include <string.h>

#define BULLET "\xE2\x80\xA2 "

/* TRUE when out ends with a bullet and at most whitespace after it, which
 * is then dropped, so the text that comes next stays on the bullet's line
 * ("<li>\n<br>Text"). */
static gboolean
trim_to_bullet (GString *out)
{
  gsize len = out->len;
  while (len > 0 && g_ascii_isspace (out->str[len - 1]))
    len--;
  if (len < 3 || memcmp (out->str + len - 3, BULLET, 3) != 0)
    return FALSE;
  g_string_truncate (out, len);
  g_string_append_c (out, ' ');
  return TRUE;
}

/* Drops paragraphs that repeat an earlier one word for word: some of
 * GOG's descriptions have their feature list twice. */
static gchar *
drop_repeated_paragraphs (const char *text)
{
  gchar **paragraphs = g_strsplit (text, "\n\n", -1);
  GPtrArray *kept = g_ptr_array_new ();
  for (gchar **p = paragraphs; *p; p++)
    if (!g_ptr_array_find_with_equal_func (kept, *p, g_str_equal, NULL))
      g_ptr_array_add (kept, *p);
  g_ptr_array_add (kept, NULL);
  gchar *out = g_strjoinv ("\n\n", (gchar **) kept->pdata);
  g_ptr_array_unref (kept);
  g_strfreev (paragraphs);
  return out;
}

static void
append_unescaped_entity (GString *out, const char *entity, gsize len)
{
  if (g_ascii_strncasecmp (entity, "&amp;", len) == 0 && len == 5)
    g_string_append_c (out, '&');
  else if (g_ascii_strncasecmp (entity, "&lt;", len) == 0 && len == 4)
    g_string_append_c (out, '<');
  else if (g_ascii_strncasecmp (entity, "&gt;", len) == 0 && len == 4)
    g_string_append_c (out, '>');
  else if (g_ascii_strncasecmp (entity, "&quot;", len) == 0 && len == 6)
    g_string_append_c (out, '"');
  else if (g_ascii_strncasecmp (entity, "&#39;", len) == 0 && len == 5)
    g_string_append_c (out, '\'');
  else if (g_ascii_strncasecmp (entity, "&apos;", len) == 0 && len == 6)
    g_string_append_c (out, '\'');
  else if (g_ascii_strncasecmp (entity, "&nbsp;", len) == 0 && len == 6)
    g_string_append_c (out, ' ');
  else if (len > 3 && entity[1] == '#')
    {
      /* numeric character reference: &#8217; or &#x2019; */
      gboolean hex = entity[2] == 'x' || entity[2] == 'X';
      const char *digits = entity + (hex ? 3 : 2);
      gchar *end = NULL;
      guint64 cp = g_ascii_strtoull (digits, &end, hex ? 16 : 10);
      if (end == entity + len - 1 && end != digits && cp > 0 && cp <= 0x10FFFF &&
          !(cp >= 0xD800 && cp <= 0xDFFF))
        g_string_append_unichar (out, (gunichar) cp);
      else
        g_string_append_len (out, entity, len);
    }
  else
    g_string_append_len (out, entity, len);
}

gchar *
text_strip_html (const char *html)
{
  if (!html || !*html)
    return g_strdup ("");

  GString *out = g_string_new (NULL);
  const char *p = html;

  while (*p)
    {
      if (*p == '<')
        {
          const char *end = strchr (p, '>');
          gsize taglen = end ? (gsize) (end - p + 1) : strlen (p);

          if (g_ascii_strncasecmp (p, "<br", 3) == 0 && trim_to_bullet (out))
            ; /* "<li><br>Text": the text stays on the bullet's line */
          else if (g_ascii_strncasecmp (p, "<br", 3) == 0 ||
                   g_ascii_strncasecmp (p, "<li", 3) == 0)
            {
              if (out->len > 0 && out->str[out->len - 1] != '\n')
                g_string_append_c (out, '\n');
              if (g_ascii_strncasecmp (p, "<li", 3) == 0)
                g_string_append (out, BULLET);
            }
          else if (g_ascii_strncasecmp (p, "<p", 2) == 0 ||
                   g_ascii_strncasecmp (p, "</p", 3) == 0 ||
                   g_ascii_strncasecmp (p, "</ul", 4) == 0 ||
                   g_ascii_strncasecmp (p, "</li", 4) == 0)
            {
              if (out->len > 0 && out->str[out->len - 1] != '\n')
                g_string_append_c (out, '\n');
            }

          p += taglen;
          if (!end)
            break;
          continue;
        }
      else if (*p == '&')
        {
          const char *end = strchr (p, ';');
          if (end && (gsize) (end - p) < 10)
            {
              append_unescaped_entity (out, p, (gsize) (end - p + 1));
              p = end + 1;
              continue;
            }
          g_string_append_c (out, *p);
          p++;
        }
      else
        {
          g_string_append_c (out, *p);
          p++;
        }
    }

  /* One space between words and none around line breaks (GOG's HTML
   * indents and double-spaces), at most one empty line in a row, trimmed
   * edges. GRegex refuses invalid UTF-8 (product JSON isn't checked), so
   * mend it first. */
  gchar *valid = g_utf8_make_valid (out->str, (gssize) out->len);
  g_string_free (out, TRUE);
  static const char *const rules[][2] = {
    { "\r", "" },
    { "[ \t]+", " " },
    { " ?\n ?", "\n" },
    { "\n{3,}", "\n\n" },
  };
  gchar *collapsed = valid;
  for (guint i = 0; i < G_N_ELEMENTS (rules); i++)
    {
      GRegex *re = g_regex_new (rules[i][0], 0, 0, NULL);
      gchar *next = g_regex_replace (re, collapsed, -1, 0, rules[i][1], 0, NULL);
      g_regex_unref (re);
      if (next)
        {
          g_free (collapsed);
          collapsed = next;
        }
    }
  gchar *trimmed = drop_repeated_paragraphs (g_strstrip (collapsed));
  g_free (collapsed);
  return trimmed;
}

gchar *
text_format_size (guint64 bytes)
{
  return g_format_size (bytes);
}

/* English names for the language names GOG writes in other scripts. */
static const struct
{
  const char *native;
  const char *english;
} native_languages[] = {
  { "русский", "Russian" },          { "українська", "Ukrainian" },
  { "беларуская", "Belarusian" },    { "български", "Bulgarian" },
  { "српски", "Serbian" },           { "македонски", "Macedonian" },
  { "ελληνικά", "Greek" },           { "العربية", "Arabic" },
  { "עברית", "Hebrew" },             { "فارسی", "Persian" },
  { "ไทย", "Thai" },                 { "한국어", "Korean" },
  { "日本語", "Japanese" },          { "中文", "Chinese" },
  { "简体中文", "Simplified Chinese" }, { "中文(简体)", "Simplified Chinese" },
  { "繁體中文", "Traditional Chinese" }, { "中文(繁體)", "Traditional Chinese" },
  { "हिन्दी", "Hindi" },              { "tiếng việt", "Vietnamese" },
};

static gboolean
has_non_latin_letter (const char *s)
{
  /* g_utf8_next_char() could step past the end of a broken sequence. */
  if (!g_utf8_validate (s, -1, NULL))
    return FALSE;
  for (const char *p = s; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (!g_unichar_isalpha (c))
        continue;
      GUnicodeScript script = g_unichar_get_script (c);
      if (script != G_UNICODE_SCRIPT_LATIN && script != G_UNICODE_SCRIPT_COMMON &&
          script != G_UNICODE_SCRIPT_INHERITED)
        return TRUE;
    }
  return FALSE;
}

gchar *
text_language_label (const char *name)
{
  if (!name || !has_non_latin_letter (name))
    return g_strdup (name ? name : "");

  /* GOG writes Ukrainian with a Latin "y" in front of the Cyrillic. */
  gchar *fixed = g_str_has_prefix (name, "yкраїн") ? g_strconcat ("у", name + 1, NULL)
                                                   : g_strdup (name);
  gchar *folded = g_utf8_casefold (fixed, -1);
  const char *english = NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (native_languages) && !english; i++)
    {
      gchar *key = g_utf8_casefold (native_languages[i].native, -1);
      if (g_strcmp0 (folded, key) == 0)
        english = native_languages[i].english;
      g_free (key);
    }
  g_free (folded);

  /* Unknown: as GOG writes it (capitalising some scripts, like Georgian,
   * would change their letters). */
  if (!english)
    return fixed;

  /* Capitalised like a name: "Українська", as English names are. */
  gunichar first = g_utf8_get_char (fixed);
  gchar upper[8] = { 0 };
  g_unichar_to_utf8 (g_unichar_totitle (first), upper);
  gchar *out = g_strdup_printf ("%s%s (%s)", upper, g_utf8_next_char (fixed), english);
  g_free (fixed);
  return out;
}
