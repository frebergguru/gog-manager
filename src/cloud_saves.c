/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "cloud_saves.h"
#include "config.h"
#include "fetch.h"
#include "json_util.h"
#include <curl/curl.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <utime.h>

/* The client lgogdownloader logs in to GOG Galaxy as. */
#define GALAXY_CLIENT_ID "46899977096215655"
/* What GOG Galaxy asks remote-config for. */
#define COMPONENT_VERSION "2.0.51"
/* Times this close are the same: file systems and GOG keep them differently. */
#define SAME_TIME 2
/* A file being fetched, next to where it goes. */
#define PART_SUFFIX ".gog-manager-part"
#define BACKUPS_KEPT 10

G_DEFINE_QUARK (gog-cloud-error-quark, gog_cloud_error)

struct _GogCloudSession
{
  gchar *client_id;
  gchar *user_id;
  gchar *token;      /* the game client's access token; never written anywhere */
  GStrv locations;   /* name, folder, name, folder... */
};

void
gog_cloud_file_free (GogCloudFile *file)
{
  if (!file)
    return;
  g_free (file->name);
  g_free (file->local_path);
  g_free (file->cloud_hash);
  g_free (file);
}

void
gog_cloud_session_free (GogCloudSession *session)
{
  if (!session)
    return;
  g_free (session->client_id);
  g_free (session->user_id);
  if (session->token)
    memset (session->token, 0, strlen (session->token));
  g_free (session->token);
  g_strfreev (session->locations);
  g_free (session);
}

/* ---- HTTP ---- */

static size_t
write_bytes (char *data, size_t size, size_t n, void *user_data)
{
  g_byte_array_append (user_data, (const guint8 *) data, size * n);
  return size * n;
}

static size_t
write_file (char *data, size_t size, size_t n, void *user_data)
{
  return fwrite (data, size, n, user_data) * size;
}

typedef struct
{
  const guint8 *data;
  gsize len;
  gsize pos;
} Upload;

static size_t
read_upload (char *buffer, size_t size, size_t n, void *user_data)
{
  Upload *up = user_data;
  gsize count = MIN (size * n, up->len - up->pos);
  memcpy (buffer, up->data + up->pos, count);
  up->pos += count;
  return count;
}

static int
check_cancelled (void *user_data, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
  (void) a;
  (void) b;
  (void) c;
  (void) d;
  return g_cancellable_is_cancelled (user_data) ? 1 : 0;
}

/* One request. GET into *body (when body is not NULL) or into the file
 * out; PUT with upload. bearer and headers may be NULL. FALSE with error
 * set when nothing usable came back; *code gets the HTTP status. */
static gboolean
http (const char *method, const char *url, const char *bearer, const char *const *headers,
      const guint8 *upload, gsize upload_len, GByteArray **body, FILE *out, long *code,
      GCancellable *cancellable, GError **error)
{
  static gsize once;
  if (g_once_init_enter (&once))
    {
      curl_global_init (CURL_GLOBAL_DEFAULT);
      g_once_init_leave (&once, 1);
    }

  *code = 0;
  CURL *curl = curl_easy_init ();
  if (!curl)
    {
      g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not start libcurl");
      return FALSE;
    }
  struct curl_slist *list = NULL;
  gchar *auth = bearer ? g_strdup_printf ("Authorization: Bearer %s", bearer) : NULL;
  if (auth)
    list = curl_slist_append (list, auth);
  for (const char *const *h = headers; h && *h; h++)
    list = curl_slist_append (list, *h);

  GByteArray *bytes = g_byte_array_new ();
  Upload up = { upload, upload_len, 0 };
  curl_easy_setopt (curl, CURLOPT_URL, url);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt (curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt (curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt (curl, CURLOPT_PROTOCOLS, (long) CURLPROTO_HTTPS);
  curl_easy_setopt (curl, CURLOPT_REDIR_PROTOCOLS, (long) CURLPROTO_HTTPS);
#endif
  curl_easy_setopt (curl, CURLOPT_USERAGENT, "gog-manager");
  curl_easy_setopt (curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 30L);
  /* Stalled for a minute: give up. */
  curl_easy_setopt (curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt (curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, check_cancelled);
  curl_easy_setopt (curl, CURLOPT_XFERINFODATA, cancellable);
  const char *ca = gog_fetch_ca_bundle ();
  if (ca)
    curl_easy_setopt (curl, CURLOPT_CAINFO, ca);
  if (g_str_equal (method, "PUT"))
    {
      curl_easy_setopt (curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt (curl, CURLOPT_READFUNCTION, read_upload);
      curl_easy_setopt (curl, CURLOPT_READDATA, &up);
      curl_easy_setopt (curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) upload_len);
    }
  else
    /* Saves Galaxy sent compressed come back as they were. */
    curl_easy_setopt (curl, CURLOPT_ACCEPT_ENCODING, "");
  if (out)
    {
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_file);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA, out);
    }
  else
    {
      curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, write_bytes);
      curl_easy_setopt (curl, CURLOPT_WRITEDATA, bytes);
    }

  CURLcode res = curl_easy_perform (curl);
  curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, code);
  curl_easy_cleanup (curl);
  curl_slist_free_all (list);
  if (auth)
    memset (auth, 0, strlen (auth));
  g_free (auth);

  if (res != CURLE_OK)
    {
      g_byte_array_unref (bytes);
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return FALSE;
      g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not reach GOG: %s",
                   curl_easy_strerror (res));
      return FALSE;
    }
  if (body)
    *body = bytes;
  else
    g_byte_array_unref (bytes);
  return TRUE;
}

/* A GET whose answer is JSON text; NULL with error set unless it came back
 * with 200. Transfer full. */
static gchar *
get_text (const char *url, const char *bearer, const char *what, GCancellable *cancellable,
          long *code, GError **error)
{
  const char *headers[] = { "Accept: application/json", NULL };
  GByteArray *body = NULL;
  long status = 0;
  if (!http ("GET", url, bearer, headers, NULL, 0, &body, NULL, &status, cancellable, error))
    return NULL;
  if (code)
    *code = status;
  if (status != 200)
    {
      g_byte_array_unref (body);
      g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "GOG did not give %s (HTTP %ld).",
                   what, status);
      return NULL;
    }
  g_byte_array_append (body, (const guint8 *) "", 1);
  return (gchar *) g_byte_array_free (body, FALSE);
}

/* ---- the parts ---- */

gchar *
gog_cloud_parse_builds (const char *json)
{
  json_object *root = ju_parse_string (json);
  json_object *items = ju_get_array (root, "items");
  const char *first = NULL, *main = NULL;
  for (gsize i = 0; items && i < json_object_array_length (items) && !main; i++)
    {
      json_object *item = json_object_array_get_idx (items, i);
      const char *link = ju_get_string (item, "link", NULL);
      if (ju_get_int64 (item, "generation", 0) != 2 || !link)
        continue;
      if (!first)
        first = link;
      /* The default branch has none; others are betas and the like. */
      if (!ju_get_string (item, "branch", NULL))
        main = link;
    }
  gchar *link = g_strdup (main ? main : first);
  if (root)
    json_object_put (root);
  return link;
}

gboolean
gog_cloud_parse_manifest (const guint8 *data, gsize len, gchar **client_id, gchar **client_secret)
{
  *client_id = *client_secret = NULL;
  gchar *text = NULL;
  if (len >= 2 && data[0] == 0x78)
    {
      GZlibDecompressor *z = g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
      GInputStream *raw = g_memory_input_stream_new_from_data (data, len, NULL);
      GInputStream *in = g_converter_input_stream_new (raw, G_CONVERTER (z));
      GOutputStream *mem = g_memory_output_stream_new_resizable ();
      if (g_output_stream_splice (mem, in, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, NULL) >= 0)
        {
          g_output_stream_write (mem, "", 1, NULL, NULL);
          text = g_memory_output_stream_steal_data (G_MEMORY_OUTPUT_STREAM (mem));
        }
      g_object_unref (mem);
      g_object_unref (in);
      g_object_unref (raw);
      g_object_unref (z);
    }
  if (!text)
    text = g_strndup ((const char *) data, len);
  json_object *root = ju_parse_string (text);
  g_free (text);
  const char *id = ju_get_string (root, "clientId", NULL);
  const char *secret = ju_get_string (root, "clientSecret", NULL);
  if (id && *id && secret && *secret)
    {
      *client_id = g_strdup (id);
      *client_secret = g_strdup (secret);
    }
  if (root)
    json_object_put (root);
  return *client_id != NULL;
}

GStrv
gog_cloud_parse_locations (const char *json, const char *client_id)
{
  json_object *root = ju_parse_string (json);
  json_object *storage = ju_get_obj (ju_get_obj (ju_get_obj (root, "content"), "Windows"),
                                     "cloudStorage");
  GStrv result = NULL;
  if (ju_get_bool (storage, "enabled", FALSE))
    {
      GPtrArray *out = g_ptr_array_new ();
      json_object *locations = ju_get_array (storage, "locations");
      for (gsize i = 0; locations && i < json_object_array_length (locations); i++)
        {
          json_object *loc = json_object_array_get_idx (locations, i);
          const char *name = ju_get_string (loc, "name", NULL);
          const char *where = ju_get_string (loc, "location", NULL);
          if (name && *name && where && *where && gog_cloud_name_is_safe (name) && !strchr (name, '/'))
            {
              g_ptr_array_add (out, g_strdup (name));
              g_ptr_array_add (out, g_strdup (where));
            }
        }
      /* Galaxy's own place for the game's saves when it names none. */
      if (out->len == 0)
        {
          g_ptr_array_add (out, g_strdup ("__default"));
          g_ptr_array_add (out, g_strdup_printf ("<?APPLICATION_DATA_LOCAL?>/GOG.com/Galaxy/"
                                                 "Applications/%s/Storage",
                                                 client_id));
        }
      g_ptr_array_add (out, NULL);
      result = (GStrv) g_ptr_array_free (out, FALSE);
    }
  if (root)
    json_object_put (root);
  return result;
}

/* The user folder under drive_c/users: wanted when it is there, else the
 * one that is (Wine names it after the login, Proton "steamuser"). */
static gchar *
profile_dir (const GogCloudGame *game)
{
  gchar *users = g_build_filename (game->wine_root, "drive_c", "users", NULL);
  gchar *dir = g_build_filename (users, game->wine_user, NULL);
  if (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      GDir *d = g_dir_open (users, 0, NULL);
      const char *name;
      while (d && (name = g_dir_read_name (d)))
        {
          gchar *other = g_build_filename (users, name, NULL);
          if (g_strcmp0 (name, "Public") != 0 && g_file_test (other, G_FILE_TEST_IS_DIR))
            {
              g_free (dir);
              dir = other;
              break;
            }
          g_free (other);
        }
      if (d)
        g_dir_close (d);
    }
  g_free (users);
  return dir;
}

/* path with each part that exists in another case ("appdata" for
 * "AppData") as it is on disk, as Windows would find it. */
static gchar *
match_case (const char *path)
{
  gchar **parts = g_strsplit (path, "/", -1);
  GString *out = g_string_new (path[0] == '/' ? "/" : "");
  gboolean exists = TRUE;
  for (gchar **p = parts; *p; p++)
    {
      if (!**p)
        continue;
      gchar *name = g_strdup (*p);
      gchar *candidate = g_build_filename (out->str, name, NULL);
      if (exists && !g_file_test (candidate, G_FILE_TEST_EXISTS))
        {
          GDir *d = g_dir_open (out->len ? out->str : ".", 0, NULL);
          const char *entry;
          gboolean found = FALSE;
          while (d && (entry = g_dir_read_name (d)) && !found)
            if (g_ascii_strcasecmp (entry, name) == 0)
              {
                g_free (name);
                name = g_strdup (entry);
                found = TRUE;
              }
          if (d)
            g_dir_close (d);
          exists = found;
        }
      g_free (candidate);
      if (out->len && out->str[out->len - 1] != '/')
        g_string_append_c (out, '/');
      g_string_append (out, name);
      g_free (name);
    }
  g_strfreev (parts);
  return g_string_free (out, FALSE);
}

gchar *
gog_cloud_resolve_location (const char *location, const GogCloudGame *game)
{
  gchar *text = g_strdup (location);
  g_strdelimit (text, "\\", '/');
  gchar *base = NULL;
  const char *rest = text;
  if (g_str_has_prefix (text, "<?"))
    {
      const char *end = strstr (text, "?>");
      if (!end)
        {
          g_free (text);
          return NULL;
        }
      gchar *var = g_strndup (text + 2, end - text - 2);
      rest = end + 2;
      gchar *profile = profile_dir (game);
      static const struct
      {
        const char *var;
        const char *under_profile;
      } vars[] = {
        { "DOCUMENTS", "Documents" },
        { "APPLICATION_DATA_ROAMING", "AppData/Roaming" },
        { "APPLICATION_DATA_LOCAL", "AppData/Local" },
        { "APPLICATION_DATA_LOCAL_LOW", "AppData/LocalLow" },
        { "SAVED_GAMES", "Saved Games" },
      };
      if (g_str_equal (var, "INSTALL"))
        base = g_strdup (game->install_dir);
      for (guint i = 0; i < G_N_ELEMENTS (vars) && !base; i++)
        if (g_str_equal (var, vars[i].var))
          base = g_build_filename (profile, vars[i].under_profile, NULL);
      g_free (profile);
      g_free (var);
      if (!base)
        {
          g_free (text);
          return NULL;
        }
    }
  else
    {
      /* A plain Windows path: under the prefix's drive C. */
      if (g_ascii_strncasecmp (rest, "C:", 2) != 0)
        {
          g_free (text);
          return NULL;
        }
      base = g_build_filename (game->wine_root, "drive_c", NULL);
      rest += 2;
    }
  gchar *joined = g_build_filename (base, rest, NULL);
  gchar *path = match_case (joined);
  g_free (joined);
  g_free (base);
  g_free (text);
  return path;
}

gboolean
gog_cloud_name_is_safe (const char *name)
{
  if (!name || !*name || name[0] == '/' || strchr (name, '\\'))
    return FALSE;
  gchar **parts = g_strsplit (name, "/", -1);
  gboolean safe = TRUE;
  for (gchar **p = parts; *p && safe; p++)
    safe = **p && !g_str_equal (*p, ".") && !g_str_equal (*p, "..");
  g_strfreev (parts);
  return safe;
}

static gint64
parse_time (const char *text)
{
  if (!text)
    return 0;
  GTimeZone *utc = g_time_zone_new_utc ();
  GDateTime *dt = g_date_time_new_from_iso8601 (text, utc);
  g_time_zone_unref (utc);
  if (!dt)
    return 0;
  gint64 t = g_date_time_to_unix (dt);
  g_date_time_unref (dt);
  return t;
}

static gint
compare_files (gconstpointer a, gconstpointer b)
{
  return g_strcmp0 ((*(GogCloudFile *const *) a)->name, (*(GogCloudFile *const *) b)->name);
}

GPtrArray *
gog_cloud_parse_listing (const char *json)
{
  GPtrArray *files = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_cloud_file_free);
  json_object *root = ju_parse_string (json);
  for (gsize i = 0; root && json_object_is_type (root, json_type_array) &&
                    i < json_object_array_length (root);
       i++)
    {
      json_object *entry = json_object_array_get_idx (root, i);
      const char *name = ju_get_string (entry, "name", NULL);
      if (!gog_cloud_name_is_safe (name) || !strchr (name, '/'))
        continue;
      GogCloudFile *file = g_new0 (GogCloudFile, 1);
      file->name = g_strdup (name);
      file->state = GOG_CLOUD_ONLY_ON_GOG;
      file->cloud_size = (guint64) ju_get_int64 (entry, "bytes", 0);
      file->cloud_mtime = parse_time (ju_get_string (entry, "last_modified", NULL));
      const char *hash = ju_get_string (entry, "hash", NULL);
      file->cloud_hash = hash && *hash ? g_ascii_strdown (hash, -1) : NULL;
      g_ptr_array_add (files, file);
    }
  if (root)
    json_object_put (root);
  g_ptr_array_sort (files, compare_files);
  return files;
}

GogCloudState
gog_cloud_compare (gint64 local_mtime, guint64 local_size, const char *local_md5,
                   gint64 cloud_mtime, guint64 cloud_size, const char *cloud_hash)
{
  if (local_md5 && cloud_hash && g_ascii_strcasecmp (local_md5, cloud_hash) == 0)
    return GOG_CLOUD_IN_SYNC;
  /* GOG keeps Galaxy's saves gzipped, so its size and checksum are of the
   * packed file: only the time tells. A file fetched or sent gets GOG's. */
  (void) local_size;
  (void) cloud_size;
  gint64 diff = local_mtime - cloud_mtime;
  if (diff > SAME_TIME)
    return GOG_CLOUD_NEWER_HERE;
  if (diff < -SAME_TIME)
    return GOG_CLOUD_NEWER_ON_GOG;
  return GOG_CLOUD_IN_SYNC;
}

/* ---- connecting ---- */

static gchar *
tokens_file (void)
{
  return g_build_filename (g_get_user_config_dir (), "lgogdownloader", "galaxy_tokens.json", NULL);
}

/* lgogdownloader's GOG Galaxy refresh token and user id. */
static gboolean
read_galaxy_login (gchar **refresh_token, gchar **user_id, GError **error)
{
  gchar *path = tokens_file ();
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  const char *client = ju_get_string (root, "client_id", NULL);
  const char *refresh = ju_get_string (root, "refresh_token", NULL);
  /* A fresh login has no client_id; lgogdownloader adds it when it renews
   * the login. Another one is a game's (see read_galaxy_login's error). */
  gboolean ok = refresh && *refresh && (!client || g_strcmp0 (client, GALAXY_CLIENT_ID) == 0);
  if (ok)
    {
      *refresh_token = g_strdup (refresh);
      *user_id = g_strdup (ju_get_string (root, "user_id", NULL));
    }
  else
    g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NEEDS_LOGIN,
                         root && refresh ? "lgogdownloader's GOG login only works for one game "
                                           "now (lgogdownloader's own cloud-save commands do "
                                           "that). Log in to GOG again."
                                         : "lgogdownloader is not logged in to GOG. Log in to "
                                           "GOG first.");
  if (root)
    json_object_put (root);
  return ok;
}

GogCloudSession *
gog_cloud_connect (const GogCloudGame *game, GCancellable *cancellable, GError **error)
{
  gchar *refresh = NULL, *user_id = NULL;
  if (!read_galaxy_login (&refresh, &user_id, error))
    return NULL;

  GogCloudSession *session = g_new0 (GogCloudSession, 1);
  session->user_id = user_id;
  gchar *client_secret = NULL;
  gboolean ok = FALSE;

  /* 1. The game's client, from its newest Galaxy build. */
  gchar *url = g_strdup_printf ("https://content-system.gog.com/products/%" G_GINT64_FORMAT
                                "/os/windows/builds?generation=2",
                                game->product_id);
  gchar *builds = get_text (url, NULL, "the game's builds", cancellable, NULL, error);
  g_free (url);
  gchar *manifest_url = builds ? gog_cloud_parse_builds (builds) : NULL;
  g_free (builds);
  if (builds && !manifest_url && error && !*error)
    g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NOT_SUPPORTED,
                         "This game has no GOG Galaxy build for Windows, so GOG keeps no cloud "
                         "saves for it.");
  if (manifest_url)
    {
      GByteArray *body = NULL;
      long code = 0;
      if (http ("GET", manifest_url, NULL, NULL, NULL, 0, &body, NULL, &code, cancellable, error))
        {
          if (code == 200 &&
              gog_cloud_parse_manifest (body->data, body->len, &session->client_id, &client_secret))
            ok = TRUE;
          else
            g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED,
                         "Could not read the game's build manifest (HTTP %ld).", code);
          g_byte_array_unref (body);
        }
      g_free (manifest_url);
    }

  /* 2. A login for the game's client, from the Galaxy one. Not a new
   * session, so lgogdownloader's login stays as it is. */
  if (ok)
    {
      ok = FALSE;
      gchar *token_url = g_strdup_printf ("https://auth.gog.com/token?client_id=%s&client_secret=%s"
                                          "&grant_type=refresh_token&refresh_token=%s"
                                          "&without_new_session=1",
                                          session->client_id, client_secret, refresh);
      long code = 0;
      GError *token_error = NULL;
      gchar *answer = get_text (token_url, NULL, "a login for the game", cancellable, &code,
                                &token_error);
      memset (token_url, 0, strlen (token_url));
      g_free (token_url);
      json_object *root = answer ? ju_parse_string (answer) : NULL;
      const char *token = ju_get_string (root, "access_token", NULL);
      if (token && *token)
        {
          session->token = g_strdup (token);
          const char *uid = ju_get_string (root, "user_id", NULL);
          if (uid && *uid)
            {
              g_free (session->user_id);
              session->user_id = g_strdup (uid);
            }
          ok = session->user_id != NULL;
          if (!ok)
            g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED,
                                 "GOG's login gave no user id.");
        }
      else if (code == 400 || code == 401 || code == 403)
        g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NEEDS_LOGIN,
                             "GOG did not accept lgogdownloader's login for this game's cloud "
                             "saves. Log in to GOG again.");
      else if (token_error)
        g_propagate_error (error, g_steal_pointer (&token_error));
      else
        g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED,
                             "GOG's login answer could not be read.");
      g_clear_error (&token_error);
      if (root)
        json_object_put (root);
      if (answer)
        memset (answer, 0, strlen (answer));
      g_free (answer);
    }

  /* 3. Where the saves are. */
  if (ok)
    {
      ok = FALSE;
      gchar *config_url = g_strdup_printf ("https://remote-config.gog.com/components/galaxy_client/"
                                           "clients/%s?component_version=" COMPONENT_VERSION,
                                           session->client_id);
      gchar *config = get_text (config_url, NULL, "where the game keeps its saves", cancellable,
                                NULL, error);
      g_free (config_url);
      GStrv templates = config ? gog_cloud_parse_locations (config, session->client_id) : NULL;
      g_free (config);
      if (config && !templates && error && !*error)
        g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NOT_SUPPORTED,
                             "GOG keeps no cloud saves for this game.");
      if (templates)
        {
          GPtrArray *locations = g_ptr_array_new ();
          for (GStrv t = templates; t[0] && t[1]; t += 2)
            {
              gchar *folder = gog_cloud_resolve_location (t[1], game);
              if (!folder)
                {
                  g_warning ("Cloud saves: can't place %s (%s)", t[0], t[1]);
                  continue;
                }
              g_ptr_array_add (locations, g_strdup (t[0]));
              g_ptr_array_add (locations, folder);
            }
          g_ptr_array_add (locations, NULL);
          session->locations = (GStrv) g_ptr_array_free (locations, FALSE);
          ok = session->locations[0] != NULL;
          if (!ok)
            g_set_error_literal (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_NOT_SUPPORTED,
                                 "GOG keeps this game's saves in places a Wine prefix doesn't "
                                 "have.");
          g_strfreev (templates);
        }
    }

  memset (refresh, 0, strlen (refresh));
  g_free (refresh);
  if (client_secret)
    memset (client_secret, 0, strlen (client_secret));
  g_free (client_secret);
  if (!ok)
    {
      gog_cloud_session_free (session);
      return NULL;
    }
  return session;
}

/* ---- listing ---- */

/* The folder of the location a name is under; NULL if none. */
static const char *
location_folder (GogCloudSession *session, const char *name)
{
  const char *slash = strchr (name, '/');
  if (!slash)
    return NULL;
  for (GStrv l = session->locations; l[0] && l[1]; l += 2)
    if (strlen (l[0]) == (gsize) (slash - name) && strncmp (l[0], name, slash - name) == 0)
      return l[1];
  return NULL;
}

static gchar *
file_md5 (const char *path)
{
  gchar *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, NULL))
    return NULL;
  gchar *md5 = g_compute_checksum_for_data (G_CHECKSUM_MD5, (const guchar *) data, len);
  g_free (data);
  return md5;
}

static void
add_local_files (GHashTable *by_name, GPtrArray *files, const char *name_prefix, const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *entry;
  while (d && (entry = g_dir_read_name (d)))
    {
      gchar *path = g_build_filename (dir, entry, NULL);
      gchar *name = g_strdup_printf ("%s/%s", name_prefix, entry);
      GStatBuf st;
      if (g_lstat (path, &st) == 0 && S_ISDIR (st.st_mode))
        add_local_files (by_name, files, name, path);
      else if (g_stat (path, &st) == 0 && S_ISREG (st.st_mode) && !g_str_has_suffix (entry, PART_SUFFIX))
        {
          GogCloudFile *file = g_hash_table_lookup (by_name, name);
          if (!file)
            {
              file = g_new0 (GogCloudFile, 1);
              file->name = g_strdup (name);
              file->state = GOG_CLOUD_ONLY_HERE;
              g_ptr_array_add (files, file);
              g_hash_table_insert (by_name, file->name, file);
            }
          file->local_size = st.st_size;
          file->local_mtime = st.st_mtime;
        }
      g_free (name);
      g_free (path);
    }
  if (d)
    g_dir_close (d);
}

GPtrArray *
gog_cloud_list (GogCloudSession *session, GCancellable *cancellable, GError **error)
{
  gchar *url = g_strdup_printf ("https://cloudstorage.gog.com/v1/%s/%s", session->user_id,
                                session->client_id);
  gchar *json = get_text (url, session->token, "the list of saves", cancellable, NULL, error);
  g_free (url);
  if (!json)
    return NULL;
  GPtrArray *files = gog_cloud_parse_listing (json);
  g_free (json);

  GHashTable *by_name = g_hash_table_new (g_str_hash, g_str_equal);
  for (guint i = 0; i < files->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (files, i);
      g_hash_table_insert (by_name, file->name, file);
    }
  for (GStrv l = session->locations; l[0] && l[1]; l += 2)
    add_local_files (by_name, files, l[0], l[1]);
  g_hash_table_unref (by_name);

  for (guint i = 0; i < files->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (files, i);
      const char *folder = location_folder (session, file->name);
      if (folder)
        file->local_path = g_build_filename (folder, strchr (file->name, '/') + 1, NULL);
      if (file->cloud_mtime > 0 && file->local_mtime > 0)
        {
          gchar *md5 = file->cloud_hash ? file_md5 (file->local_path) : NULL;
          file->state = gog_cloud_compare (file->local_mtime, file->local_size, md5,
                                           file->cloud_mtime, file->cloud_size, file->cloud_hash);
          g_free (md5);
        }
    }
  g_ptr_array_sort (files, compare_files);
  return files;
}

/* ---- moving files ---- */

static gchar *
file_url (GogCloudSession *session, const char *name)
{
  gchar *escaped = g_uri_escape_string (name, "/", FALSE);
  gchar *url = g_strdup_printf ("https://cloudstorage.gog.com/v1/%s/%s/%s", session->user_id,
                                session->client_id, escaped);
  g_free (escaped);
  return url;
}

static void
set_mtime (const char *path, gint64 mtime)
{
  struct utimbuf times = { (time_t) mtime, (time_t) mtime };
  if (utime (path, &times) != 0)
    g_warning ("Could not set the time of %s: %s", path, g_strerror (errno));
}

static gboolean
copy_file (const char *from, const char *to, GError **error)
{
  gchar *dir = g_path_get_dirname (to);
  g_mkdir_with_parents (dir, 0700);
  g_free (dir);
  GFile *src = g_file_new_for_path (from);
  GFile *dst = g_file_new_for_path (to);
  gboolean ok = g_file_copy (src, dst, G_FILE_COPY_OVERWRITE | G_FILE_COPY_ALL_METADATA, NULL,
                             NULL, NULL, error);
  g_object_unref (src);
  g_object_unref (dst);
  return ok;
}

/* Removes dir and everything in it. */
static void
remove_tree (const char *dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const char *name;
  while (d && (name = g_dir_read_name (d)))
    {
      gchar *path = g_build_filename (dir, name, NULL);
      if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
        remove_tree (path);
      else
        g_unlink (path);
      g_free (path);
    }
  if (d)
    g_dir_close (d);
  g_rmdir (dir);
}

gchar *
gog_cloud_backup_dir (gint64 product_id)
{
  gchar *base = gog_config_app_file ("cloud-save-backups");
  gchar *id = g_strdup_printf ("%" G_GINT64_FORMAT, product_id);
  gchar *game_dir = g_build_filename (base, id, NULL);
  g_free (id);
  g_free (base);

  /* Only the newest ones. */
  GPtrArray *old = g_ptr_array_new_with_free_func (g_free);
  GDir *d = g_dir_open (game_dir, 0, NULL);
  const char *entry;
  while (d && (entry = g_dir_read_name (d)))
    g_ptr_array_add (old, g_strdup (entry));
  if (d)
    g_dir_close (d);
  g_ptr_array_sort (old, (GCompareFunc) g_strcmp0);
  for (guint i = 0; old->len >= BACKUPS_KEPT && i + BACKUPS_KEPT <= old->len; i++)
    {
      gchar *path = g_build_filename (game_dir, g_ptr_array_index (old, i), NULL);
      remove_tree (path);
      g_free (path);
    }
  g_ptr_array_unref (old);

  GDateTime *now = g_date_time_new_now_local ();
  gchar *stamp = g_date_time_format (now, "%Y-%m-%d_%H-%M-%S");
  g_date_time_unref (now);
  gchar *dir = g_build_filename (game_dir, stamp, NULL);
  g_free (stamp);
  g_free (game_dir);
  return dir;
}

gboolean
gog_cloud_download (GogCloudSession *session, GPtrArray *files, const char *backup_dir,
                    GogCloudProgress progress, gpointer user_data, GCancellable *cancellable,
                    GError **error)
{
  for (guint i = 0; i < files->len; i++)
    {
      GogCloudFile *file = g_ptr_array_index (files, i);
      if (progress)
        progress (i, files->len, file->name, user_data);
      if (!file->local_path || !file->cloud_mtime)
        continue;
      gchar *dir = g_path_get_dirname (file->local_path);
      if (g_mkdir_with_parents (dir, 0755) != 0)
        {
          g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not make %s: %s",
                       dir, g_strerror (errno));
          g_free (dir);
          return FALSE;
        }
      g_free (dir);
      gchar *part = g_strconcat (file->local_path, PART_SUFFIX, NULL);
      FILE *out = g_fopen (part, "wb");
      if (!out)
        {
          g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not write %s: %s",
                       part, g_strerror (errno));
          g_free (part);
          return FALSE;
        }
      gchar *url = file_url (session, file->name);
      long code = 0;
      gboolean ok = http ("GET", url, session->token, NULL, NULL, 0, NULL, out, &code, cancellable,
                          error);
      g_free (url);
      ok = fclose (out) == 0 && ok;
      if (ok && code != 200)
        {
          ok = FALSE;
          g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED,
                       "GOG did not give %s (HTTP %ld).", file->name, code);
        }
      /* The one here is kept before it is replaced. */
      if (ok && backup_dir && g_file_test (file->local_path, G_FILE_TEST_EXISTS))
        {
          gchar *backup = g_build_filename (backup_dir, file->name, NULL);
          ok = copy_file (file->local_path, backup, error);
          g_free (backup);
        }
      if (ok && g_rename (part, file->local_path) != 0)
        {
          ok = FALSE;
          g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not replace %s: %s",
                       file->local_path, g_strerror (errno));
        }
      if (ok)
        set_mtime (file->local_path, file->cloud_mtime);
      else
        g_unlink (part);
      g_free (part);
      if (!ok)
        return FALSE;
    }
  return TRUE;
}

static GBytes *
gzip_bytes (const gchar *data, gsize len)
{
  GZlibCompressor *z = g_zlib_compressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP, -1);
  GInputStream *raw = g_memory_input_stream_new_from_data (data, len, NULL);
  GInputStream *in = g_converter_input_stream_new (raw, G_CONVERTER (z));
  GOutputStream *mem = g_memory_output_stream_new_resizable ();
  g_output_stream_splice (mem, in, G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, NULL);
  GBytes *packed = g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (mem));
  g_object_unref (mem);
  g_object_unref (in);
  g_object_unref (raw);
  g_object_unref (z);
  return packed;
}

gboolean
gog_cloud_upload (GogCloudSession *session, GPtrArray *files, GogCloudProgress progress,
                  gpointer user_data, GCancellable *cancellable, GError **error)
{
  GHashTable *sent = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  gboolean ok = TRUE;
  for (guint i = 0; i < files->len && ok; i++)
    {
      GogCloudFile *file = g_ptr_array_index (files, i);
      if (progress)
        progress (i, files->len, file->name, user_data);
      if (!file->local_path)
        continue;
      gchar *data = NULL;
      gsize len = 0;
      GStatBuf st;
      if (g_stat (file->local_path, &st) != 0 ||
          !g_file_get_contents (file->local_path, &data, &len, error))
        {
          if (error && !*error)
            g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED, "Could not read %s",
                         file->local_path);
          ok = FALSE;
          break;
        }
      /* Gzipped, as GOG Galaxy sends them, which unpacks them by the
       * Content-Encoding when it fetches them. */
      GBytes *packed = gzip_bytes (data, len);
      gsize packed_len = 0;
      const guint8 *packed_data = g_bytes_get_data (packed, &packed_len);
      gchar *md5 = g_compute_checksum_for_data (G_CHECKSUM_MD5, packed_data, packed_len);
      GDateTime *mtime = g_date_time_new_from_unix_utc (st.st_mtime);
      gchar *when = g_date_time_format (mtime, "%Y-%m-%dT%H:%M:%S+00:00");
      g_date_time_unref (mtime);
      gchar *etag = g_strdup_printf ("Etag: %s", md5);
      gchar *modified = g_strdup_printf ("X-Object-Meta-LocalLastModified: %s", when);
      const char *headers[] = { "Content-Type: application/octet-stream", "Content-Encoding: gzip",
                                etag, modified, NULL };
      gchar *url = file_url (session, file->name);
      long code = 0;
      ok = http ("PUT", url, session->token, headers, packed_data, packed_len, NULL, NULL, &code,
                 cancellable, error);
      if (ok && (code < 200 || code >= 300))
        {
          ok = FALSE;
          g_set_error (error, GOG_CLOUD_ERROR, GOG_CLOUD_ERROR_FAILED,
                       "GOG did not take %s (HTTP %ld).", file->name, code);
        }
      if (ok)
        {
          /* Size and time as sent, to tell whether it changed since. */
          gint64 *stamp = g_new (gint64, 2);
          stamp[0] = st.st_size;
          stamp[1] = st.st_mtime;
          g_hash_table_insert (sent, g_strdup (file->name), stamp);
        }
      g_free (url);
      g_free (modified);
      g_free (etag);
      g_free (when);
      g_free (md5);
      g_bytes_unref (packed);
      g_free (data);
    }

  /* The files sent take GOG's time, so they compare as the same later on
   * even where GOG's copy has no checksum to compare. */
  if (g_hash_table_size (sent) > 0)
    {
      GError *list_error = NULL;
      GPtrArray *now = gog_cloud_list (session, cancellable, &list_error);
      for (guint i = 0; now && i < now->len; i++)
        {
          GogCloudFile *file = g_ptr_array_index (now, i);
          gint64 *stamp = g_hash_table_lookup (sent, file->name);
          if (stamp && file->cloud_mtime > 0 && file->local_path &&
              (gint64) file->local_size == stamp[0] && file->local_mtime == stamp[1])
            set_mtime (file->local_path, file->cloud_mtime);
        }
      if (now)
        g_ptr_array_unref (now);
      if (list_error)
        g_warning ("Cloud saves: %s", list_error->message);
      g_clear_error (&list_error);
    }
  g_hash_table_unref (sent);
  return ok;
}
