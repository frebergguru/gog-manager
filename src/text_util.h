/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_TEXT_UTIL_H
#define GOG_TEXT_UTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* Convert GOG's loose HTML description markup to plain, readable text:
 * <br>/<p> become newlines, <li> becomes a bullet, all other tags are
 * dropped, and common HTML entities are unescaped. Returns a newly
 * allocated string (never NULL; empty string for NULL/empty input). */
gchar *text_strip_html (const char *html);

/* A changelog's HTML as Pango markup for a label: version headings
 * (<h1>-<h4>) large and bold, section headings (<h5>, <h6>) bold, one
 * bullet per line with nested lists indented, empty items dropped, and
 * <strong>, <em> and <code> kept. Whitespace is HTML's: line breaks in the
 * source are spaces. Never NULL; empty when there is no text. */
gchar *text_changelog_markup (const char *html);

/* Human-readable file size, e.g. "1.2 GB". */
gchar *text_format_size (guint64 bytes);

/* A language name as GOG writes it, for display: one in a non-Latin script
 * gets its English name too, "Українська (Ukrainian)"; Latin ones ("polski")
 * stay as they are. Transfer full. */
gchar *text_language_label (const char *name);

G_END_DECLS

#endif
