/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_FETCH_H
#define GOG_FETCH_H

#include <glib.h>

G_BEGIN_DECLS

/* Downloads urls[i] to dests[i] with one curl run, a few at a time, with a
 * plain, generic User-Agent (nothing that identifies the user). codes[i]
 * gets the HTTP status, 0 when nothing came back; a file that didn't come
 * back with 200 should not be trusted. *curl_error (may be NULL) gets
 * curl's first error line, or NULL. Blocking. FALSE, with error set, only
 * when curl could not be run at all. */
gboolean gog_fetch_files (const char *const *urls, const char *const *dests, guint n, int *codes,
                          gchar **curl_error, GError **error);

/* Removes a folder and the files in it (not subfolders). */
void gog_fetch_remove_dir (const char *dir);

G_END_DECLS

#endif
