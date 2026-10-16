/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "text_util.h"
#include <string.h>

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

          if (g_ascii_strncasecmp (p, "<br", 3) == 0 ||
              g_ascii_strncasecmp (p, "<li", 3) == 0)
            {
              if (out->len > 0 && out->str[out->len - 1] != '\n')
                g_string_append_c (out, '\n');
              if (g_ascii_strncasecmp (p, "<li", 3) == 0)
                g_string_append (out, "\xE2\x80\xA2 "); /* bullet */
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

  /* collapse 3+ consecutive newlines down to 2, trim edges */
  GRegex *re = g_regex_new ("\n{3,}", 0, 0, NULL);
  gchar *collapsed = g_regex_replace (re, out->str, -1, 0, "\n\n", 0, NULL);
  g_regex_unref (re);
  g_string_free (out, TRUE);
  gchar *trimmed = g_strdup (g_strstrip (collapsed));
  g_free (collapsed);
  return trimmed;
}

gchar *
text_format_size (guint64 bytes)
{
  return g_format_size (bytes);
}
