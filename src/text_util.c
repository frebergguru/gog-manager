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

/* ---- changelogs ---- */

typedef enum
{
  BLOCK_VERSION, /* <h1>-<h4> */
  BLOCK_SECTION, /* <h5>, <h6> */
  BLOCK_BULLET,
  BLOCK_PARAGRAPH,
  BLOCK_RULE,    /* <hr>: only spacing */
} BlockKind;

typedef struct
{
  BlockKind kind;
  int depth;          /* list nesting, for bullets */
  GString *markup;
  gboolean has_text;
  gboolean last_space;
  char open[8];       /* inline tags open in it: 'b', 'i', 't', 'a' (a link) */
  guint n_open;
} Block;

typedef struct
{
  GPtrArray *blocks; /* Block*, the finished ones with text */
  Block *cur;
  int depth;
} Changelog;

static void
block_free (Block *b)
{
  g_string_free (b->markup, TRUE);
  g_free (b);
}

static const char *
closer (char tag)
{
  return tag == 'b' ? "</b>" : tag == 'i' ? "</i>" : tag == 'a' ? "</a>" : "</tt>";
}

static void
end_block (Changelog *c)
{
  Block *b = c->cur;
  if (!b)
    return;
  c->cur = NULL;
  if (!b->has_text && b->kind != BLOCK_RULE)
    {
      block_free (b);
      return;
    }
  if (b->last_space)
    g_string_truncate (b->markup, b->markup->len - 1);
  while (b->n_open > 0)
    {
      char t = b->open[--b->n_open];
      g_string_append (b->markup, closer (t));
    }
  g_ptr_array_add (c->blocks, b);
}

static Block *
start_block (Changelog *c, BlockKind kind)
{
  end_block (c);
  Block *b = g_new0 (Block, 1);
  b->kind = kind;
  b->depth = MAX (c->depth, 1);
  b->markup = g_string_new (NULL);
  c->cur = b;
  return b;
}

static void
add_text (Changelog *c, const char *text, gsize len)
{
  Block *b = c->cur ? c->cur : start_block (c, BLOCK_PARAGRAPH);
  for (gsize i = 0; i < len; i++)
    {
      char ch = text[i];
      if (g_ascii_isspace (ch))
        {
          if (b->has_text && !b->last_space)
            {
              g_string_append_c (b->markup, ' ');
              b->last_space = TRUE;
            }
          continue;
        }
      switch (ch)
        {
        case '&': g_string_append (b->markup, "&amp;"); break;
        case '<': g_string_append (b->markup, "&lt;"); break;
        case '>': g_string_append (b->markup, "&gt;"); break;
        case '"': g_string_append (b->markup, "&quot;"); break;
        case '\'': g_string_append (b->markup, "&#39;"); break;
        default: g_string_append_c (b->markup, ch);
        }
      b->has_text = TRUE;
      b->last_space = FALSE;
    }
}

/* href: for a link ('a'), already escaped for markup. */
static void
open_inline (Changelog *c, char tag, const char *href)
{
  Block *b = c->cur ? c->cur : start_block (c, BLOCK_PARAGRAPH);
  if (b->n_open >= G_N_ELEMENTS (b->open))
    return;
  b->open[b->n_open++] = tag;
  if (tag == 'a')
    g_string_append_printf (b->markup, "<a href=\"%s\">", href);
  else
    g_string_append (b->markup, tag == 'b' ? "<b>" : tag == 'i' ? "<i>" : "<tt>");
}

/* The href of an <a> tag, escaped for markup; NULL unless it is a web
 * address. attrs runs to the tag's '>'. */
static gchar *
link_target (const char *attrs, gsize len)
{
  gchar *tag = g_strndup (attrs, len);
  gchar *found = NULL;
  const char *h = strstr (tag, "href=");
  if (h && (h[5] == '"' || h[5] == '\''))
    {
      char quote = h[5];
      const char *start = h + 6;
      const char *end = strchr (start, quote);
      if (end)
        {
          gchar *url = g_strndup (start, (gsize) (end - start));
          if (g_str_has_prefix (url, "https://") || g_str_has_prefix (url, "http://"))
            found = g_markup_escape_text (url, -1);
          g_free (url);
        }
    }
  g_free (tag);
  return found;
}

static void
close_inline (Changelog *c, char tag)
{
  Block *b = c->cur;
  /* Only the innermost: Pango wants them nested. */
  if (!b || b->n_open == 0 || b->open[b->n_open - 1] != tag)
    return;
  b->n_open--;
  if (b->last_space)
    {
      g_string_truncate (b->markup, b->markup->len - 1);
      b->last_space = FALSE;
      g_string_append (b->markup, closer (tag));
      g_string_append_c (b->markup, ' ');
      b->last_space = TRUE;
    }
  else
    g_string_append (b->markup, closer (tag));
}

static gboolean
tag_is (const char *name, gsize len, const char *want)
{
  return len == strlen (want) && g_ascii_strncasecmp (name, want, len) == 0;
}

static void
handle_tag (Changelog *c, const char *name, gsize len, gboolean closing, const char *attrs,
            gsize attrs_len)
{
  if (len == 2 && (name[0] == 'h' || name[0] == 'H') && name[1] >= '1' && name[1] <= '6')
    {
      if (closing)
        end_block (c);
      else
        start_block (c, name[1] <= '4' ? BLOCK_VERSION : BLOCK_SECTION);
    }
  else if (tag_is (name, len, "ul") || tag_is (name, len, "ol"))
    {
      end_block (c);
      c->depth = MAX (c->depth + (closing ? -1 : 1), 0);
    }
  else if (tag_is (name, len, "li"))
    {
      if (closing)
        end_block (c);
      else
        start_block (c, BLOCK_BULLET);
    }
  else if (tag_is (name, len, "p") || tag_is (name, len, "div") || tag_is (name, len, "pre"))
    {
      /* "<li><p>Text</p></li>" stays a bullet. */
      gboolean in_bullet = c->cur && c->cur->kind == BLOCK_BULLET;
      if (!closing && !(in_bullet && !c->cur->has_text))
        start_block (c, BLOCK_PARAGRAPH);
      else if (closing && !in_bullet)
        end_block (c);
    }
  else if (tag_is (name, len, "br"))
    {
      if (c->cur && c->cur->has_text)
        {
          if (c->cur->last_space)
            g_string_truncate (c->cur->markup, c->cur->markup->len - 1);
          g_string_append_c (c->cur->markup, '\n');
          /* What follows starts the line, like after a block's start. */
          c->cur->last_space = TRUE;
        }
    }
  else if (tag_is (name, len, "hr"))
    {
      start_block (c, BLOCK_RULE);
      end_block (c);
    }
  else if (tag_is (name, len, "strong") || tag_is (name, len, "b"))
    closing ? close_inline (c, 'b') : open_inline (c, 'b', NULL);
  else if (tag_is (name, len, "em") || tag_is (name, len, "i"))
    closing ? close_inline (c, 'i') : open_inline (c, 'i', NULL);
  else if (tag_is (name, len, "code"))
    closing ? close_inline (c, 't') : open_inline (c, 't', NULL);
  else if (tag_is (name, len, "a"))
    {
      if (closing)
        close_inline (c, 'a');
      else
        {
          /* Only links to web pages; a link without one is just text. */
          gchar *href = link_target (attrs, attrs_len);
          if (href)
            open_inline (c, 'a', href);
          g_free (href);
        }
    }
}

gchar *
text_changelog_markup (const char *html)
{
  if (!html || !*html)
    return g_strdup ("");
  gchar *valid = g_utf8_make_valid (html, -1);
  Changelog c = { g_ptr_array_new_with_free_func ((GDestroyNotify) block_free), NULL, 0 };
  for (const char *p = valid; *p;)
    {
      if (*p == '<')
        {
          const char *end = strchr (p, '>');
          if (!end)
            break;
          const char *name = p + 1;
          gboolean closing = *name == '/';
          if (closing)
            name++;
          gsize len = 0;
          while (g_ascii_isalnum (name[len]))
            len++;
          handle_tag (&c, name, len, closing, name + len, (gsize) (end - (name + len)));
          p = end + 1;
        }
      else if (*p == '&')
        {
          const char *end = strchr (p, ';');
          GString *plain = g_string_new (NULL);
          if (end && (gsize) (end - p) < 10)
            {
              append_unescaped_entity (plain, p, (gsize) (end - p + 1));
              p = end + 1;
            }
          else
            {
              g_string_append_c (plain, '&');
              p++;
            }
          /* A no-break space is still a space between words. */
          if (g_str_equal (plain->str, "\xC2\xA0"))
            add_text (&c, " ", 1);
          else
            add_text (&c, plain->str, plain->len);
          g_string_free (plain, TRUE);
        }
      else
        {
          const char *next = p;
          while (*next && *next != '<' && *next != '&')
            next++;
          add_text (&c, p, (gsize) (next - p));
          p = next;
        }
    }
  end_block (&c);
  g_free (valid);

  GString *out = g_string_new (NULL);
  BlockKind prev = BLOCK_RULE;
  gboolean first = TRUE;
  for (guint i = 0; i < c.blocks->len; i++)
    {
      Block *b = g_ptr_array_index (c.blocks, i);
      if (b->kind == BLOCK_RULE)
        {
          prev = BLOCK_RULE;
          continue;
        }
      if (!first)
        {
          /* Bullets one per line, and a heading right above what it heads;
           * an empty line everywhere else. */
          gboolean tight = (prev == BLOCK_BULLET && b->kind == BLOCK_BULLET) ||
                           prev == BLOCK_VERSION || prev == BLOCK_SECTION;
          g_string_append (out, tight ? "\n" : "\n\n");
        }
      first = FALSE;
      switch (b->kind)
        {
        case BLOCK_VERSION:
          g_string_append_printf (out, "<span size=\"large\" weight=\"bold\">%s</span>",
                                  b->markup->str);
          break;
        case BLOCK_SECTION:
          g_string_append_printf (out, "<b>%s</b>", b->markup->str);
          break;
        case BLOCK_BULLET:
          for (int d = 1; d < b->depth; d++)
            g_string_append (out, "    ");
          g_string_append (out, b->depth > 1 ? "\xE2\x97\xA6 " : BULLET);
          g_string_append (out, b->markup->str);
          break;
        default:
          g_string_append (out, b->markup->str);
        }
      prev = b->kind;
    }
  g_ptr_array_unref (c.blocks);
  return g_string_free (out, FALSE);
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
          else if ((g_ascii_strncasecmp (p, "<h", 2) == 0 && g_ascii_isdigit (p[2])) ||
                   g_ascii_strncasecmp (p, "<hr", 3) == 0)
            {
              /* A heading or a rule starts a section: an empty line
               * before it (changelogs: "<h4>1.2 (2025-05-12)</h4>"). */
              while (out->len > 0 && out->str[out->len - 1] == '\n')
                g_string_truncate (out, out->len - 1);
              if (out->len > 0)
                g_string_append (out, "\n\n");
            }
          else if (g_ascii_strncasecmp (p, "<p", 2) == 0 ||
                   (g_ascii_strncasecmp (p, "</h", 3) == 0 && g_ascii_isdigit (p[3])) ||
                   g_ascii_strncasecmp (p, "</ol", 4) == 0 ||
                   g_ascii_strncasecmp (p, "<div", 4) == 0 ||
                   g_ascii_strncasecmp (p, "</div", 5) == 0 ||
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
    /* A line that is only "product_feature_1390579243" or
     * "product_description_2015545325": names of parts GOG's store page
     * fills in itself, not text (DOOM (2016), DOOM I Enhanced). */
    { "(?m)^(" BULLET ")?product_[a-z]+_[0-9]+$", "" },
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
