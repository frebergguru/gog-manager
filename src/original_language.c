/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "original_language.h"
#include "config.h"
#include "gog_sync.h"
#include "json_util.h"
#include <gio/gio.h>
#include <string.h>

#define CACHE_FILE "gog-manager-original-languages.json"
#define SPARQL_URL "https://query.wikidata.org/sparql"
#define BATCH 80
/* Games Wikidata didn't know are asked about again after this long. */
#define RETRY_SECONDS (30 * 24 * 3600)

/* The main language(s) of a country, by ISO 3166 code, as lgogdownloader
 * codes. Wikidata's "official language" can't be used directly: for the
 * United States it gives Spanish. Countries with several languages keep
 * them all. */
static const struct
{
  const char *country;
  const char *codes;
} country_languages[] = {
  { "US", "en" },    { "GB", "en" },       { "IE", "en" },       { "AU", "en" },
  { "NZ", "en" },    { "IS", "en" },       { "CA", "en+fr" },    { "FR", "fr" },
  { "BE", "nl+fr" }, { "CH", "de+fr+it" }, { "LU", "fr+de" },    { "DE", "de" },
  { "AT", "de" },    { "NL", "nl" },       { "IT", "it" },       { "ES", "es" },
  { "PT", "pt" },    { "BR", "br" },       { "MX", "es_mx" },    { "AR", "es_mx" },
  { "CL", "es_mx" }, { "PL", "pl" },       { "CZ", "cz" },       { "SK", "sk" },
  { "HU", "hu" },    { "RO", "ro" },       { "BG", "bl" },       { "RU", "ru" },
  { "BY", "ru" },    { "UA", "uk" },       { "NO", "no" },       { "SE", "sv" },
  { "DK", "da" },    { "FI", "fi" },       { "TR", "tr" },       { "IL", "he" },
  { "JP", "jp" },    { "KR", "ko" },       { "CN", "cn" },       { "TW", "cn" },
  { "HK", "cn" },    { "TH", "th" },       { "EG", "ar" },       { "SA", "ar" },
  { "AE", "ar" },
};

/* ISO 639-1 codes (Wikidata P218) as lgogdownloader codes, where they
 * differ or matter. */
static const struct
{
  const char *iso;
  const char *code;
} iso_languages[] = {
  { "en", "en" }, { "de", "de" }, { "fr", "fr" }, { "pl", "pl" }, { "ru", "ru" },
  { "zh", "cn" }, { "cs", "cz" }, { "es", "es" }, { "hu", "hu" }, { "it", "it" },
  { "ja", "jp" }, { "tr", "tr" }, { "pt", "pt" }, { "ko", "ko" }, { "nl", "nl" },
  { "sv", "sv" }, { "no", "no" }, { "nb", "no" }, { "nn", "no" }, { "da", "da" },
  { "fi", "fi" }, { "sk", "sk" }, { "bg", "bl" }, { "uk", "uk" }, { "ar", "ar" },
  { "ro", "ro" }, { "he", "he" }, { "th", "th" },
};

static G_LOCK_DEFINE (cache);

static guint
codes_to_bits (const char *codes)
{
  guint n = 0;
  const GogSyncLanguage *langs = gog_sync_get_languages (&n);
  guint mask = 0;
  gchar **parts = g_strsplit (codes ? codes : "", "+", -1);
  for (gchar **p = parts; *p; p++)
    for (guint i = 0; i < n; i++)
      if (g_strcmp0 (*p, langs[i].code) == 0)
        mask |= 1u << i;
  g_strfreev (parts);
  return mask;
}

static gchar *
bits_to_codes (guint mask)
{
  guint n = 0;
  const GogSyncLanguage *langs = gog_sync_get_languages (&n);
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < n; i++)
    if (mask & (1u << i))
      g_string_append_printf (out, "%s%s", out->len ? "+" : "", langs[i].code);
  return g_string_free (out, FALSE);
}

guint
gog_original_country_languages (const char *iso_country)
{
  for (guint i = 0; i < G_N_ELEMENTS (country_languages); i++)
    if (g_strcmp0 (iso_country, country_languages[i].country) == 0)
      return codes_to_bits (country_languages[i].codes);
  return 0;
}

static guint
iso_language_bits (const char *iso)
{
  for (guint i = 0; i < G_N_ELEMENTS (iso_languages); i++)
    if (g_strcmp0 (iso, iso_languages[i].iso) == 0)
      return codes_to_bits (iso_languages[i].code);
  return 0;
}

static json_object *
load_cache (void)
{
  gchar *path = gog_config_app_file (CACHE_FILE);
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (!root || !json_object_is_type (root, json_type_object))
    {
      if (root)
        json_object_put (root);
      root = json_object_new_object ();
    }
  return root;
}

static void
save_cache (json_object *root)
{
  gchar *path = gog_config_app_file (CACHE_FILE);
  GError *error = NULL;
  if (!g_file_set_contents (path, json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY),
                            -1, &error))
    {
      g_warning ("Could not save original languages: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
}

guint
gog_original_languages (const char *gamename, gchar **source)
{
  G_LOCK (cache);
  json_object *root = load_cache ();
  json_object *entry = ju_get_obj (root, gamename);
  guint mask = codes_to_bits (ju_get_string (entry, "languages", ""));
  if (source)
    *source = mask ? g_strdup (ju_get_string (entry, "source", NULL)) : NULL;
  json_object_put (root);
  G_UNLOCK (cache);
  return mask;
}

GHashTable *
gog_original_languages_load (void)
{
  GHashTable *all = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  G_LOCK (cache);
  json_object *root = load_cache ();
  if (root && json_object_is_type (root, json_type_object))
    {
      json_object_object_foreach (root, gamename, entry)
        {
          guint mask = codes_to_bits (ju_get_string (entry, "languages", ""));
          if (mask)
            g_hash_table_insert (all, g_strdup (gamename), GUINT_TO_POINTER (mask));
        }
    }
  if (root)
    json_object_put (root);
  G_UNLOCK (cache);
  return all;
}

/* What Wikidata says about one game while its rows are gathered. */
typedef struct
{
  guint original;      /* from P364 */
  guint country;       /* from P495 */
  guint developer;     /* from P178 -> P17 */
  GPtrArray *countries; /* names, for the source text */
  GPtrArray *developers;
} Found;

static void
found_free (Found *f)
{
  g_ptr_array_unref (f->countries);
  g_ptr_array_unref (f->developers);
  g_free (f);
}

static const char *
binding (json_object *row, const char *name)
{
  return ju_get_string (ju_get_obj (row, name), "value", NULL);
}

static void
add_unique (GPtrArray *list, const char *value)
{
  if (!value)
    return;
  for (guint i = 0; i < list->len; i++)
    if (g_strcmp0 (g_ptr_array_index (list, i), value) == 0)
      return;
  g_ptr_array_add (list, g_strdup (value));
}

static gchar *
join (GPtrArray *list, const char *sep)
{
  GString *out = g_string_new (NULL);
  for (guint i = 0; i < list->len; i++)
    g_string_append_printf (out, "%s%s", i ? sep : "", (const char *) g_ptr_array_index (list, i));
  return g_string_free (out, FALSE);
}

typedef struct
{
  gboolean done;
  gchar *out;
  gchar *err;
  GError *error;
} CurlRun;

static void
on_curl_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  CurlRun *run = user_data;
  g_subprocess_communicate_utf8_finish (G_SUBPROCESS (source), res, &run->out, &run->err,
                                        &run->error);
  run->done = TRUE;
}

static gboolean
wake_up (gpointer data)
{
  (void) data;
  return G_SOURCE_CONTINUE;
}

/* Runs curl and collects its output like g_spawn_sync(), but kills it as
 * soon as *cancelled is set, so Cancel doesn't wait out --max-time. */
static gboolean
run_curl (gchar **argv, const volatile gboolean *cancelled, gchar **out, gchar **err,
          gboolean *succeeded, GError **error)
{
  GSubprocess *proc = g_subprocess_newv ((const gchar *const *) argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                             G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                         error);
  if (!proc)
    return FALSE;
  GMainContext *context = g_main_context_new ();
  g_main_context_push_thread_default (context);
  CurlRun run = { 0 };
  g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_curl_done, &run);
  /* Looks at the flag a few times a second while curl runs. */
  GSource *tick = g_timeout_source_new (200);
  g_source_set_callback (tick, wake_up, NULL, NULL);
  g_source_attach (tick, context);
  while (!run.done)
    {
      if (cancelled && *cancelled)
        g_subprocess_force_exit (proc);
      g_main_context_iteration (context, TRUE);
    }
  g_source_destroy (tick);
  g_source_unref (tick);
  g_main_context_pop_thread_default (context);
  g_main_context_unref (context);

  gboolean ok = run.error == NULL;
  if (!ok)
    g_propagate_error (error, run.error);
  else if (cancelled && *cancelled)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ok = FALSE;
    }
  *succeeded = ok && g_subprocess_get_successful (proc);
  *out = run.out;
  *err = run.err;
  g_object_unref (proc);
  return ok;
}

/* Runs one SPARQL query for up to BATCH games and records the answers. */
static gboolean
lookup_batch (const char *const *names, guint n, json_object *cache,
              const volatile gboolean *cancelled, GError **error)
{
  GString *values = g_string_new (NULL);
  for (guint i = 0; i < n; i++)
    g_string_append_printf (values, " \"game/%s\"", names[i]);
  gchar *query = g_strdup_printf (
      "SELECT ?gog ?origIso ?countryLabel ?countryIso ?devLabel ?devIso WHERE {"
      " VALUES ?gog {%s}"
      " ?item wdt:P2725 ?gog ."
      " OPTIONAL { ?item wdt:P364 ?orig . ?orig wdt:P218 ?origIso }"
      " OPTIONAL { ?item wdt:P495 ?country . OPTIONAL { ?country wdt:P297 ?countryIso } }"
      " OPTIONAL { ?item wdt:P178 ?dev . ?dev wdt:P17 ?devCountry ."
      "            ?devCountry wdt:P297 ?devIso }"
      " SERVICE wikibase:label { bd:serviceParam wikibase:language \"en\". } }",
      values->str);
  g_string_free (values, TRUE);
  gchar *data = g_strconcat ("query=", query, NULL);
  g_free (query);

  /* A plain, generic User-Agent: nothing that identifies the user. */
  gchar *argv[] = { "curl", "-fsS", "--max-time", "45", "-A", "gog-manager",
                    "-H", "Accept: application/sparql-results+json",
                    "--data-urlencode", data, SPARQL_URL, NULL };
  gchar *out = NULL;
  gchar *err = NULL;
  gboolean succeeded = FALSE;
  gboolean ok = run_curl (argv, cancelled, &out, &err, &succeeded, error);
  g_free (data);
  if (ok && !succeeded)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not look up original languages on Wikidata: %s",
                   err && *err ? g_strstrip (err) : "curl failed");
      ok = FALSE;
    }
  json_object *root = ok ? ju_parse_string (out) : NULL;
  g_free (out);
  g_free (err);
  if (ok && !root)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Wikidata gave an answer that could not be read");
      ok = FALSE;
    }
  if (!ok)
    return FALSE;

  GHashTable *found = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify) found_free);
  json_object *rows = ju_get_array (ju_get_obj (root, "results"), "bindings");
  for (size_t i = 0; rows && i < json_object_array_length (rows); i++)
    {
      json_object *row = json_object_array_get_idx (rows, i);
      const char *gog = binding (row, "gog");
      if (!gog || !g_str_has_prefix (gog, "game/"))
        continue;
      Found *f = g_hash_table_lookup (found, gog + 5);
      if (!f)
        {
          f = g_new0 (Found, 1);
          f->countries = g_ptr_array_new_with_free_func (g_free);
          f->developers = g_ptr_array_new_with_free_func (g_free);
          g_hash_table_insert (found, g_strdup (gog + 5), f);
        }
      f->original |= iso_language_bits (binding (row, "origIso"));
      if (binding (row, "countryLabel"))
        {
          f->country |= gog_original_country_languages (binding (row, "countryIso"));
          add_unique (f->countries, binding (row, "countryLabel"));
        }
      guint dev = gog_original_country_languages (binding (row, "devIso"));
      if (dev)
        {
          f->developer |= dev;
          add_unique (f->developers, binding (row, "devLabel"));
        }
    }
  json_object_put (root);

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  for (guint i = 0; i < n; i++)
    {
      Found *f = g_hash_table_lookup (found, names[i]);
      guint mask = 0;
      gchar *source = NULL;
      if (f && f->original)
        {
          mask = f->original;
          source = g_strdup ("original language on Wikidata");
        }
      else if (f && f->country)
        {
          mask = f->country;
          gchar *where = join (f->countries, ", ");
          source = g_strdup_printf ("made in %s", where);
          g_free (where);
        }
      else if (f && f->developer)
        {
          mask = f->developer;
          gchar *who = join (f->developers, ", ");
          source = g_strdup_printf ("developer %s", who);
          g_free (who);
        }
      json_object *entry = json_object_new_object ();
      gchar *codes = bits_to_codes (mask);
      json_object_object_add (entry, "languages", json_object_new_string (codes));
      g_free (codes);
      if (source)
        json_object_object_add (entry, "source", json_object_new_string (source));
      json_object_object_add (entry, "checked", json_object_new_int64 (now));
      json_object_object_add (cache, names[i], entry);
      g_free (source);
    }
  g_hash_table_unref (found);
  return TRUE;
}

gboolean
gog_original_lookup (const char *const *gamenames, const volatile gboolean *cancelled,
                     GError **error)
{
  G_LOCK (cache);
  json_object *cache = load_cache ();
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  GPtrArray *todo = g_ptr_array_new ();
  for (const char *const *g = gamenames; g && *g; g++)
    {
      /* Names go into the query as string literals; GOG's are plain. */
      if (!**g || strpbrk (*g, "\"\\ \n"))
        continue;
      json_object *entry = ju_get_obj (cache, *g);
      gboolean known = entry && *ju_get_string (entry, "languages", "");
      if (entry && (known || now - ju_get_int64 (entry, "checked", 0) < RETRY_SECONDS))
        continue;
      g_ptr_array_add (todo, (gpointer) *g);
    }
  json_object_put (cache);
  /* Not locked while Wikidata answers: the UI thread reads the cache. */
  G_UNLOCK (cache);

  json_object *answers = json_object_new_object ();
  gboolean ok = TRUE;
  for (guint i = 0; ok && i < todo->len && !(cancelled && *cancelled); i += BATCH)
    ok = lookup_batch ((const char *const *) todo->pdata + i, MIN (BATCH, todo->len - i),
                       answers, cancelled, error);
  /* Whatever was looked up before a failure is kept. */
  if (json_object_object_length (answers) > 0)
    {
      G_LOCK (cache);
      cache = load_cache ();
      json_object_object_foreach (answers, name, entry)
        json_object_object_add (cache, name, json_object_get (entry));
      save_cache (cache);
      json_object_put (cache);
      G_UNLOCK (cache);
    }
  json_object_put (answers);
  g_ptr_array_unref (todo);
  return ok;
}
