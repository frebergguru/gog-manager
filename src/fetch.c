/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "config.h"
#include "fetch.h"
#include <glib/gstdio.h>
#include <string.h>

/* Downloads at the same time within one curl run. */
#define PARALLEL "4"

gboolean
gog_fetch_files (const char *const *urls, const char *const *dests, guint n, int *codes,
                 gchar **curl_error, GError **error)
{
  if (curl_error)
    *curl_error = NULL;
  for (guint i = 0; i < n; i++)
    codes[i] = 0;
  if (n == 0)
    return TRUE;

  /* One line per file on stdout: "<HTTP status> <file>"; 000 when nothing
   * came back. Web addresses only, never file:// and the like. */
  static const char *const base_argv[] = { "curl", "-sS", "--max-time", "30", "-A", "gog-manager",
                                           "--proto", "=http,https", "--parallel",
                                           "--parallel-max", PARALLEL, "-w",
                                           "%{http_code} %{filename_effective}\\n" };
  GPtrArray *argv = g_ptr_array_new_with_free_func (g_free);
  for (guint i = 0; i < G_N_ELEMENTS (base_argv); i++)
    g_ptr_array_add (argv, g_strdup (base_argv[i]));
  for (guint i = 0; i < n; i++)
    {
      g_ptr_array_add (argv, g_strdup ("-o"));
      g_ptr_array_add (argv, g_strdup (dests[i]));
      /* --url: an address from GOG's answer is never read as an option. */
      g_ptr_array_add (argv, g_strdup ("--url"));
      g_ptr_array_add (argv, g_strdup (urls[i]));
    }
  g_ptr_array_add (argv, NULL);

  gchar *out = NULL;
  gchar *err = NULL;
  gboolean ok = g_spawn_sync (gog_config_launch_dir (), (gchar **) argv->pdata, NULL,
                              G_SPAWN_SEARCH_PATH, NULL, NULL,
                              &out, &err, NULL, error);
  g_ptr_array_unref (argv);

  gchar **lines = g_strsplit (ok && out ? out : "", "\n", -1);
  for (gchar **l = lines; *l; l++)
    {
      int code = 0;
      const char *space = strchr (*l, ' ');
      if (!space || sscanf (*l, "%d", &code) != 1)
        continue;
      for (guint i = 0; i < n; i++)
        if (strcmp (space + 1, dests[i]) == 0)
          codes[i] = code;
    }
  g_strfreev (lines);

  if (ok && curl_error && err)
    {
      /* curl says the same for every file; once is enough. */
      gchar *nl = strchr (g_strstrip (err), '\n');
      if (nl)
        *nl = '\0';
      if (*err)
        *curl_error = g_strdup (err);
    }
  g_free (out);
  g_free (err);
  return ok;
}

void
gog_fetch_remove_dir (const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      gchar *path = g_build_filename (dir, name, NULL);
      g_remove (path);
      g_free (path);
    }
  if (d)
    g_dir_close (d);
  g_rmdir (dir);
}
