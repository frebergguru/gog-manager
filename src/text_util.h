#ifndef GOG_TEXT_UTIL_H
#define GOG_TEXT_UTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* Convert GOG's loose HTML description markup to plain, readable text:
 * <br>/<p> become newlines, <li> becomes a bullet, all other tags are
 * dropped, and common HTML entities are unescaped. Returns a newly
 * allocated string (never NULL; empty string for NULL/empty input). */
gchar *text_strip_html (const char *html);

/* Human-readable file size, e.g. "1.2 GB". */
gchar *text_format_size (guint64 bytes);

G_END_DECLS

#endif
