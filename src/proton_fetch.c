/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "proton_fetch.h"
#include "json_util.h"
#include "text_util.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* The release list rather than /releases/latest: "latest" is whatever the
 * maintainer last flagged, so we pick the newest published release ourselves. */
#define RELEASES_API_URL \
  "https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=30"

/* Since GE-Proton10 the archives are per-architecture
 * ("GE-Proton11-9-x86_64.tar.gz"); older releases ship "<tag>.tar.gz". */
#if defined(__x86_64__)
#define ASSET_ARCH "x86_64"
#elif defined(__aarch64__)
#define ASSET_ARCH "aarch64"
#else
#define ASSET_ARCH NULL
#endif

struct _GogProtonFetchJob
{
  GObject parent_instance;

  GThread *thread;
  GMutex proc_mutex;
  GSubprocess *current_proc; /* protected by proc_mutex */
  volatile gboolean cancelled;
};

enum
{
  SIG_PROGRESS,
  SIG_LOG_LINE,
  SIG_FINISHED,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (GogProtonFetchJob, gog_proton_fetch_job, G_TYPE_OBJECT)

gchar *
proton_fetch_get_tools_dir (void)
{
  gchar *dir = g_build_filename (g_get_user_data_dir (), "gog-manager", "proton", NULL);
  g_mkdir_with_parents (dir, 0755);
  return dir;
}

static gboolean
is_safe_dir_name (const char *name)
{
  return name && *name && !strchr (name, '/') && g_strcmp0 (name, ".") != 0 &&
         g_strcmp0 (name, "..") != 0;
}

gint
proton_fetch_compare_newest_first (gconstpointer a, gconstpointer b)
{
  /* GE-Proton builds come first, newest first; anything else in the tools
   * folder (Proton-9.0, a hand-unpacked build) after them, so it is never
   * taken for the newest GE-Proton. */
  gboolean ge_a = g_str_has_prefix (a, "GE-Proton");
  gboolean ge_b = g_str_has_prefix (b, "GE-Proton");
  if (ge_a != ge_b)
    return ge_a ? -1 : 1;
  gchar *key_a = g_utf8_collate_key_for_filename (a, -1);
  gchar *key_b = g_utf8_collate_key_for_filename (b, -1);
  gint result = strcmp (key_b, key_a);
  g_free (key_a);
  g_free (key_b);
  return result;
}

static gint
compare_newest_first_indirect (gconstpointer a, gconstpointer b)
{
  return proton_fetch_compare_newest_first (*(const gchar *const *) a, *(const gchar *const *) b);
}

GPtrArray *
proton_fetch_list_installed (void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func (g_free);
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  GDir *d = g_dir_open (tools_dir, 0, NULL);
  if (d)
    {
      const char *name;
      while ((name = g_dir_read_name (d)))
        {
          gchar *proton_bin = g_build_filename (tools_dir, name, "proton", NULL);
          if (g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR))
            g_ptr_array_add (out, g_strdup (name));
          g_free (proton_bin);
        }
      g_dir_close (d);
    }
  g_free (tools_dir);
  g_ptr_array_sort (out, compare_newest_first_indirect);
  return out;
}

/* rm -rf without following symlinks (Proton builds contain some). */
static gboolean
remove_tree (const char *path, GError **error)
{
  GStatBuf st;
  if (g_lstat (path, &st) != 0)
    {
      int saved_errno = errno;
      if (saved_errno == ENOENT)
        return TRUE;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not read %s: %s", path, g_strerror (saved_errno));
      return FALSE;
    }

  if (S_ISDIR (st.st_mode))
    {
      GDir *d = g_dir_open (path, 0, error);
      if (!d)
        return FALSE;
      gboolean ok = TRUE;
      const char *name;
      while (ok && (name = g_dir_read_name (d)))
        {
          gchar *child = g_build_filename (path, name, NULL);
          ok = remove_tree (child, error);
          g_free (child);
        }
      g_dir_close (d);
      if (!ok)
        return FALSE;
      if (g_rmdir (path) != 0)
        {
          int saved_errno = errno;
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                       "Could not remove %s: %s", path, g_strerror (saved_errno));
          return FALSE;
        }
      return TRUE;
    }

  if (g_unlink (path) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not remove %s: %s", path, g_strerror (saved_errno));
      return FALSE;
    }
  return TRUE;
}

static void
remove_thread_func (GTask *task, gpointer source_object, gpointer task_data,
                    GCancellable *cancellable)
{
  (void) source_object;
  gchar **names = task_data;
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  GError *error = NULL;

  for (gchar **name = names; *name; name++)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, &error))
        break;
      /* Only ever delete something that looks like one of our builds. */
      gchar *dir = g_build_filename (tools_dir, *name, NULL);
      gchar *proton_bin = g_build_filename (dir, "proton", NULL);
      gboolean is_build = is_safe_dir_name (*name) &&
                          g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR);
      g_free (proton_bin);
      if (!is_build)
        {
          g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                       "%s is not an installed GE-Proton build", *name);
          g_free (dir);
          break;
        }
      gboolean ok = remove_tree (dir, &error);
      g_free (dir);
      if (!ok)
        break;
    }

  g_free (tools_dir);
  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

void
proton_fetch_remove_async (const char *const *names, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, proton_fetch_remove_async);
  g_task_set_task_data (task, g_strdupv ((gchar **) names), (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, remove_thread_func);
  g_object_unref (task);
}

gboolean
proton_fetch_remove_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}

/* Apparent size of everything under path, without following symlinks. */
static guint64
tree_size (const char *path)
{
  GStatBuf st;
  if (g_lstat (path, &st) != 0)
    return 0;
  if (!S_ISDIR (st.st_mode))
    return S_ISREG (st.st_mode) ? (guint64) st.st_size : 0;

  guint64 total = 0;
  GDir *d = g_dir_open (path, 0, NULL);
  if (!d)
    return 0;
  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      gchar *child = g_build_filename (path, name, NULL);
      total += tree_size (child);
      g_free (child);
    }
  g_dir_close (d);
  return total;
}

static void
measure_thread_func (GTask *task, gpointer source_object, gpointer task_data,
                     GCancellable *cancellable)
{
  (void) source_object;
  gchar **names = task_data;
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  GArray *sizes = g_array_new (FALSE, TRUE, sizeof (guint64));
  for (gchar **name = names; *name && !g_cancellable_is_cancelled (cancellable); name++)
    {
      guint64 size = 0;
      if (is_safe_dir_name (*name))
        {
          gchar *dir = g_build_filename (tools_dir, *name, NULL);
          size = tree_size (dir);
          g_free (dir);
        }
      g_array_append_val (sizes, size);
    }
  g_free (tools_dir);
  g_task_return_pointer (task, sizes, (GDestroyNotify) g_array_unref);
}

void
proton_fetch_measure_async (const char *const *names, GCancellable *cancellable,
                            GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, proton_fetch_measure_async);
  g_task_set_task_data (task, g_strdupv ((gchar **) names), (GDestroyNotify) g_strfreev);
  g_task_run_in_thread (task, measure_thread_func);
  g_object_unref (task);
}

GArray *
proton_fetch_measure_finish (GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
gog_proton_fetch_job_finalize (GObject *object)
{
  GogProtonFetchJob *self = GOG_PROTON_FETCH_JOB (object);
  g_mutex_clear (&self->proc_mutex);
  if (self->thread)
    g_thread_unref (self->thread);
  G_OBJECT_CLASS (gog_proton_fetch_job_parent_class)->finalize (object);
}

static void
gog_proton_fetch_job_class_init (GogProtonFetchJobClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_proton_fetch_job_finalize;

  signals[SIG_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                                         G_TYPE_DOUBLE, G_TYPE_STRING);
  signals[SIG_LOG_LINE] = g_signal_new ("log-line", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 2,
                                         G_TYPE_BOOLEAN, G_TYPE_STRING);
}

static void
gog_proton_fetch_job_init (GogProtonFetchJob *self)
{
  g_mutex_init (&self->proc_mutex);
}

GogProtonFetchJob *
gog_proton_fetch_job_new (void)
{
  return g_object_new (GOG_TYPE_PROTON_FETCH_JOB, NULL);
}

/* ---- marshalling signals back to the main thread (same pattern as
 * installer_engine.c's GogInstallJob) ---- */

typedef struct
{
  GogProtonFetchJob *job;
  double fraction;
  gchar *message;
} ProgressMsg;

static gboolean
emit_progress_idle (gpointer data)
{
  ProgressMsg *pm = data;
  g_signal_emit (pm->job, signals[SIG_PROGRESS], 0, pm->fraction, pm->message);
  g_free (pm->message);
  g_object_unref (pm->job);
  g_free (pm);
  return G_SOURCE_REMOVE;
}

static void
job_progress (GogProtonFetchJob *job, double fraction, const char *message)
{
  ProgressMsg *pm = g_new (ProgressMsg, 1);
  pm->job = g_object_ref (job);
  pm->fraction = fraction;
  pm->message = g_strdup (message);
  g_idle_add (emit_progress_idle, pm);
}

typedef struct
{
  GogProtonFetchJob *job;
  gchar *line;
} LogMsg;

static gboolean
emit_log_idle (gpointer data)
{
  LogMsg *lm = data;
  g_signal_emit (lm->job, signals[SIG_LOG_LINE], 0, lm->line);
  g_free (lm->line);
  g_object_unref (lm->job);
  g_free (lm);
  return G_SOURCE_REMOVE;
}

static void
job_log (GogProtonFetchJob *job, const char *line)
{
  LogMsg *lm = g_new (LogMsg, 1);
  lm->job = g_object_ref (job);
  lm->line = g_strdup (line);
  g_idle_add (emit_log_idle, lm);
}

typedef struct
{
  GogProtonFetchJob *job;
  gboolean success;
  gchar *result; /* version tag on success, error message on failure */
} FinishedMsg;

static gboolean
emit_finished_idle (gpointer data)
{
  FinishedMsg *fm = data;
  g_signal_emit (fm->job, signals[SIG_FINISHED], 0, fm->success, fm->result);
  g_free (fm->result);
  g_object_unref (fm->job);
  g_free (fm);
  return G_SOURCE_REMOVE;
}

static void
job_finished (GogProtonFetchJob *job, gboolean success, const char *result)
{
  FinishedMsg *fm = g_new (FinishedMsg, 1);
  fm->job = g_object_ref (job);
  fm->success = success;
  fm->result = g_strdup (result);
  g_idle_add (emit_finished_idle, fm);
}

/* ---- helpers ---- */

/* Runs argv to completion, capturing combined output. When job is not NULL,
 * registers the subprocess as job->current_proc while it runs so
 * gog_proton_fetch_job_cancel() can force-exit it. */
static gboolean
run_capture (GogProtonFetchJob *job, gchar **argv, gchar **out_output, GError **error)
{
  if (job && job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }

  GSubprocess *proc = g_subprocess_newv ((const gchar *const *) argv,
                                          G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                              G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                          error);
  if (!proc)
    return FALSE;

  if (job)
    {
      g_mutex_lock (&job->proc_mutex);
      job->current_proc = proc;
      /* A cancel that landed between the check above and the spawn would
       * otherwise miss this process. */
      if (job->cancelled)
        g_subprocess_force_exit (proc);
      g_mutex_unlock (&job->proc_mutex);
    }

  /* Bytes, not _utf8: tar's warnings may name files in any encoding, and
   * that must not fail an extraction that worked. */
  GBytes *bytes = NULL;
  gchar *output = NULL;
  gboolean ok = g_subprocess_communicate (proc, NULL, NULL, &bytes, NULL, error);
  if (bytes)
    {
      gsize len = 0;
      const char *data = g_bytes_get_data (bytes, &len);
      output = g_utf8_make_valid (data ? data : "", (gssize) len);
      g_bytes_unref (bytes);
    }

  if (job)
    {
      g_mutex_lock (&job->proc_mutex);
      job->current_proc = NULL;
      g_mutex_unlock (&job->proc_mutex);
    }

  if (ok && job && job->cancelled)
    {
      /* Killed by our own cancel: say so, not "terminated (signal 9)". */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ok = FALSE;
    }
  else if (ok && !g_subprocess_get_successful (proc))
    {
      ok = FALSE;
      /* get_exit_status() asserts if the process died by signal (e.g. our
       * own force_exit() on cancel) rather than a normal exit -- check
       * which one actually happened first. */
      if (g_subprocess_get_if_signaled (proc))
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s was terminated (signal %d)",
                     argv[0], g_subprocess_get_term_sig (proc));
      else
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "%s exited with status %d",
                     argv[0], g_subprocess_get_exit_status (proc));
    }

  if (out_output)
    *out_output = output;
  else
    g_free (output);

  g_object_unref (proc);
  return ok;
}

typedef struct
{
  GogProtonFetchJob *job;
  gchar *tmp_path;
  guint64 total_size;
  volatile gboolean *downloading;
} PollData;

static gpointer
poll_thread_func (gpointer data)
{
  PollData *pd = data;
  while (*pd->downloading && !pd->job->cancelled)
    {
      GStatBuf st;
      if (g_stat (pd->tmp_path, &st) == 0 && pd->total_size > 0)
        {
          double fraction = (double) st.st_size / (double) pd->total_size;
          gchar *have = text_format_size ((guint64) st.st_size);
          gchar *total = text_format_size (pd->total_size);
          gchar *msg = g_strdup_printf ("Downloading GE-Proton… %s / %s", have, total);
          job_progress (pd->job, CLAMP (fraction, 0.0, 1.0), msg);
          g_free (have);
          g_free (total);
          g_free (msg);
        }
      g_usleep (300 * 1000);
    }
  g_free (pd->tmp_path);
  g_free (pd);
  return NULL;
}

/* ---- finding the newest release ---- */

typedef struct
{
  gchar *tag;        /* e.g. "GE-Proton11-9" */
  gchar *dir_name;   /* what the archive unpacks to, e.g. "GE-Proton11-9-x86_64" */
  gchar *url;
  guint64 size;
  gchar *sha512_url; /* NULL when the release publishes no checksum */
} ReleaseInfo;

static void
release_info_free (ReleaseInfo *info)
{
  if (!info)
    return;
  g_free (info->tag);
  g_free (info->dir_name);
  g_free (info->url);
  g_free (info->sha512_url);
  g_free (info);
}

/* The newest release seen by any lookup, so Preferences can show whether an
 * update exists without asking GitHub every time it opens. */
G_LOCK_DEFINE_STATIC (latest_cache);
static gchar *latest_cache_tag;
static gchar *latest_cache_dir;
static gint64 latest_cache_time;
#define LATEST_CACHE_USECS (10 * G_TIME_SPAN_MINUTE)

static void
remember_latest (const ReleaseInfo *info)
{
  G_LOCK (latest_cache);
  g_free (latest_cache_tag);
  g_free (latest_cache_dir);
  latest_cache_tag = g_strdup (info->tag);
  latest_cache_dir = g_strdup (info->dir_name);
  latest_cache_time = g_get_monotonic_time ();
  G_UNLOCK (latest_cache);
}

static gchar *
format_local_time (const char *unix_seconds)
{
  gint64 t = g_ascii_strtoll (unix_seconds, NULL, 10);
  if (t <= 0)
    return NULL;
  GDateTime *dt = g_date_time_new_from_unix_local (t);
  if (!dt)
    return NULL;
  gchar *out = g_date_time_format (dt, "%H:%M");
  g_date_time_unref (dt);
  return out;
}

/* GETs a GitHub API URL and returns the body. Network failures and HTTP
 * errors (notably GitHub's limit on requests without an account) become
 * readable messages instead of "curl exited with status N". */
static gchar *
github_api_get (GogProtonFetchJob *job, const char *url, GError **error)
{
  gchar *argv[] = { "curl", "-sSL", "--connect-timeout", "15", "--max-time", "60", "-D", "-",
                    "-H", "Accept: application/vnd.github+json", "-H", "Cache-Control: no-cache",
                    (gchar *) url, NULL };
  gchar *output = NULL;
  GError *local_error = NULL;
  if (!run_capture (job, argv, &output, &local_error))
    {
      g_free (output);
      if ((job && job->cancelled) ||
          g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_propagate_error (error, local_error);
          return NULL;
        }
      /* Without -f, curl only fails when it gets no HTTP answer at all. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE,
                   "Could not reach GitHub. Check your internet connection. (%s)",
                   local_error->message);
      g_error_free (local_error);
      return NULL;
    }

  /* -D - puts the response headers (one block per redirect) before the body. */
  int status = 0;
  gchar *remaining = NULL;
  gchar *reset = NULL;
  const char *body = output;
  while (g_str_has_prefix (body, "HTTP/"))
    {
      const char *end = strstr (body, "\r\n\r\n");
      if (!end)
        break;
      gchar *block = g_strndup (body, end - body);
      gchar **lines = g_strsplit (block, "\r\n", -1);
      status = 0;
      sscanf (lines[0], "%*s %d", &status);
      for (gchar **line = lines + 1; *line; line++)
        {
          if (g_ascii_strncasecmp (*line, "x-ratelimit-remaining:", 22) == 0)
            {
              g_free (remaining);
              remaining = g_strstrip (g_strdup (*line + 22));
            }
          else if (g_ascii_strncasecmp (*line, "x-ratelimit-reset:", 18) == 0)
            {
              g_free (reset);
              reset = g_strstrip (g_strdup (*line + 18));
            }
        }
      g_strfreev (lines);
      g_free (block);
      body = end + 4;
    }

  gchar *result = NULL;
  if (status == 200)
    result = g_strdup (body);
  else if ((status == 403 || status == 429) &&
           (g_strcmp0 (remaining, "0") == 0 || strstr (body, "rate limit")))
    {
      gchar *when = reset ? format_local_time (reset) : NULL;
      if (when)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                     "GitHub allows about 60 checks an hour from one network without an account, "
                     "and that limit has been reached. Try again after %s.",
                     when);
      else
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                             "GitHub allows about 60 checks an hour from one network without an "
                             "account, and that limit has been reached. Try again in an hour.");
      g_free (when);
    }
  else if (status == 0)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "GitHub sent an answer that could not be read. Try again later.");
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "GitHub answered with an error (HTTP %d). Try again later.", status);

  g_free (remaining);
  g_free (reset);
  g_free (output);
  return result;
}

/* The asset called name in release, if it has a download URL. */
static json_object *
find_named_asset (json_object *release, const char *name)
{
  json_object *assets = ju_get_array (release, "assets");
  if (!assets)
    return NULL;
  size_t n = json_object_array_length (assets);
  for (size_t i = 0; i < n; i++)
    {
      json_object *asset = json_object_array_get_idx (assets, i);
      if (g_strcmp0 (ju_get_string (asset, "name", ""), name) == 0 &&
          ju_get_string (asset, "browser_download_url", NULL))
        return asset;
    }
  return NULL;
}

/* Returns the archive in release that we can run on this machine, or NULL. */
static json_object *
find_release_asset (json_object *release)
{
  const char *tag = ju_get_string (release, "tag_name", NULL);
  if (!tag)
    return NULL;
  json_object *asset = NULL;
  if (ASSET_ARCH)
    {
      gchar *arch_name = g_strdup_printf ("%s-%s.tar.gz", tag, ASSET_ARCH);
      asset = find_named_asset (release, arch_name);
      g_free (arch_name);
    }
  if (!asset)
    {
      gchar *plain_name = g_strdup_printf ("%s.tar.gz", tag);
      asset = find_named_asset (release, plain_name);
      g_free (plain_name);
    }
  return asset;
}

static ReleaseInfo *
query_newest_release (GogProtonFetchJob *job, GError **error)
{
  gchar *body = github_api_get (job, RELEASES_API_URL, error);
  if (!body)
    return NULL;

  json_object *root = json_tokener_parse (body);
  g_free (body);
  if (!root || !json_object_is_type (root, json_type_array))
    {
      if (root)
        json_object_put (root);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not read GitHub's list of GE-Proton releases");
      return NULL;
    }

  /* Newest published, non-draft, non-prerelease release that has a
   * download for this architecture. ISO 8601 timestamps compare as strings. */
  json_object *best_release = NULL, *best_asset = NULL;
  const char *best_published = NULL;
  size_t n_releases = json_object_array_length (root);
  for (size_t i = 0; i < n_releases; i++)
    {
      json_object *release = json_object_array_get_idx (root, i);
      if (ju_get_bool (release, "draft", FALSE) || ju_get_bool (release, "prerelease", FALSE))
        continue;
      const char *published = ju_get_string (release, "published_at", "");
      if (best_published && g_strcmp0 (published, best_published) <= 0)
        continue;
      json_object *asset = find_release_asset (release);
      if (!asset)
        continue;
      best_release = release;
      best_asset = asset;
      best_published = published;
    }

  if (!best_release)
    {
      json_object_put (root);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not find a GE-Proton release with a download for this computer");
      return NULL;
    }

  const char *tag = ju_get_string (best_release, "tag_name", NULL);
  const char *asset_name = ju_get_string (best_asset, "name", "");
  ReleaseInfo *info = g_new0 (ReleaseInfo, 1);
  info->tag = g_strdup (tag);
  /* The archive unpacks into a directory named after itself, which then
   * sits under the tools dir. */
  info->dir_name = g_strndup (asset_name, strlen (asset_name) - strlen (".tar.gz"));
  info->url = g_strdup (ju_get_string (best_asset, "browser_download_url", NULL));
  info->size = (guint64) ju_get_int64 (best_asset, "size", 0);
  gchar *sum_name = g_strdup_printf ("%s.sha512sum", info->dir_name);
  json_object *sum_asset = find_named_asset (best_release, sum_name);
  g_free (sum_name);
  if (sum_asset)
    info->sha512_url = g_strdup (ju_get_string (sum_asset, "browser_download_url", NULL));
  json_object_put (root);

  if (!is_safe_dir_name (info->tag) || !is_safe_dir_name (info->dir_name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GitHub returned an unexpected release name: %s",
                   info->dir_name ? info->dir_name : "");
      release_info_free (info);
      return NULL;
    }

  remember_latest (info);
  return info;
}

gboolean
proton_fetch_is_installed (const char *tag, const char *dir_name)
{
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  gchar *proton_bin = g_build_filename (tools_dir, dir_name, "proton", NULL);
  /* Releases fetched before the per-architecture archives live in <tag>/. */
  gchar *legacy_bin = g_build_filename (tools_dir, tag, "proton", NULL);
  gboolean found = g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR) ||
                   g_file_test (legacy_bin, G_FILE_TEST_IS_REGULAR);
  g_free (legacy_bin);
  g_free (proton_bin);
  g_free (tools_dir);
  return found;
}

static void
check_latest_thread_func (GTask *task, gpointer source_object, gpointer task_data,
                          GCancellable *cancellable)
{
  (void) source_object;
  (void) task_data;
  (void) cancellable;

  G_LOCK (latest_cache);
  if (latest_cache_tag && g_get_monotonic_time () - latest_cache_time < LATEST_CACHE_USECS)
    {
      gchar **result = g_new0 (gchar *, 3);
      result[0] = g_strdup (latest_cache_tag);
      result[1] = g_strdup (latest_cache_dir);
      G_UNLOCK (latest_cache);
      g_task_return_pointer (task, result, (GDestroyNotify) g_strfreev);
      return;
    }
  G_UNLOCK (latest_cache);

  GError *error = NULL;
  ReleaseInfo *info = query_newest_release (NULL, &error);
  if (!info)
    {
      g_task_return_error (task, error);
      return;
    }
  gchar **result = g_new0 (gchar *, 3);
  result[0] = g_strdup (info->tag);
  result[1] = g_strdup (info->dir_name);
  release_info_free (info);
  g_task_return_pointer (task, result, (GDestroyNotify) g_strfreev);
}

void
proton_fetch_check_latest_async (GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, proton_fetch_check_latest_async);
  g_task_run_in_thread (task, check_latest_thread_func);
  g_object_unref (task);
}

gboolean
proton_fetch_check_latest_finish (GAsyncResult *result, gchar **out_tag, gchar **out_dir_name,
                                  GError **error)
{
  gchar **pair = g_task_propagate_pointer (G_TASK (result), error);
  if (!pair)
    return FALSE;
  if (out_tag)
    *out_tag = g_strdup (pair[0]);
  if (out_dir_name)
    *out_dir_name = g_strdup (pair[1]);
  g_strfreev (pair);
  return TRUE;
}

/* ---- worker thread ---- */

/* Removes download and unpack leftovers (".gog-ge-proton-*") more than a
 * day old: what a crash or kill mid-download leaves behind. */
static void
sweep_stale_downloads (const char *tools_dir)
{
  GDir *d = g_dir_open (tools_dir, 0, NULL);
  if (!d)
    return;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      if (!g_str_has_prefix (name, ".gog-ge-proton-"))
        continue;
      gchar *path = g_build_filename (tools_dir, name, NULL);
      GStatBuf st;
      if (g_lstat (path, &st) == 0 && now - st.st_mtime > 24 * 3600)
        remove_tree (path, NULL);
      g_free (path);
    }
  g_dir_close (d);
}

/* Checks path against the SHA-512 sum GE-Proton publishes next to each
 * archive ("<hex>  <file name>"). With several lines, the one naming
 * archive_name counts. */
static gboolean
verify_sha512 (GogProtonFetchJob *job, const char *path, const char *sum_url,
               const char *archive_name, GError **error)
{
  gchar *sum_argv[] = { "curl", "-fsSL", "--retry", "3", (gchar *) sum_url, NULL };
  gchar *sum_text = NULL;
  if (!run_capture (job, sum_argv, &sum_text, error))
    {
      g_free (sum_text);
      g_prefix_error (error, "Could not download the checksum file: ");
      return FALSE;
    }

  const char *p = sum_text ? sum_text : "";
  /* The line for this archive, if the file lists more than one. */
  for (const char *line = sum_text; line && *line;)
    {
      const char *end = strchr (line, '\n');
      gsize len = end ? (gsize) (end - line) : strlen (line);
      gchar *one = g_strndup (line, len);
      gboolean names_it = g_str_has_suffix (g_strstrip (one), archive_name);
      g_free (one);
      if (names_it)
        {
          p = line;
          break;
        }
      line = end ? end + 1 : NULL;
    }
  while (*p && g_ascii_isspace (*p))
    p++;
  gsize hex_len = strspn (p, "0123456789abcdefABCDEF");
  if (hex_len != 128)
    {
      g_free (sum_text);
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "The published checksum file could not be read");
      return FALSE;
    }
  gchar *expected = g_ascii_strdown (p, 128);
  g_free (sum_text);

  FILE *f = g_fopen (path, "rb");
  if (!f)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not read the download: %s", g_strerror (saved_errno));
      g_free (expected);
      return FALSE;
    }
  GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA512);
  const gsize buf_size = 1 << 20;
  guchar *buf = g_malloc (buf_size);
  gsize n;
  gboolean ok = TRUE;
  while ((n = fread (buf, 1, buf_size, f)) > 0)
    {
      if (job->cancelled)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          ok = FALSE;
          break;
        }
      g_checksum_update (checksum, buf, n);
    }
  if (ok && ferror (f))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read the download");
      ok = FALSE;
    }
  fclose (f);
  g_free (buf);

  if (ok && g_strcmp0 (g_checksum_get_string (checksum), expected) != 0)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "The download is damaged or has been changed: it does not match the "
                           "checksum GE-Proton published. Nothing was installed.");
      ok = FALSE;
    }
  g_checksum_free (checksum);
  g_free (expected);
  return ok;
}

static gboolean
fetch_sync (GogProtonFetchJob *job, GError **error)
{
  job_progress (job, -1, "Checking for the latest GE-Proton release…");

  ReleaseInfo *rel = query_newest_release (job, error);
  if (!rel)
    return FALSE;

  gchar *line = g_strdup_printf ("Newest GE-Proton release: %s", rel->tag);
  job_log (job, line);
  g_free (line);

  if (proton_fetch_is_installed (rel->tag, rel->dir_name))
    {
      job_log (job, "Already up to date.");
      job_finished (job, TRUE, rel->tag);
      release_info_free (rel);
      return TRUE;
    }

  gboolean ok = FALSE;
  gchar *tools_dir = proton_fetch_get_tools_dir ();
  gchar *install_dir = g_build_filename (tools_dir, rel->dir_name, NULL);
  gchar *unpack_dir = NULL;
  sweep_stale_downloads (tools_dir);

  /* Download next to where it will be extracted rather than into $TMPDIR:
   * the archive is several hundred MB, too big for a tmpfs /tmp. */
  gchar *tmp_path = g_build_filename (tools_dir, ".gog-ge-proton-XXXXXX.tar.gz", NULL);
  gint tmp_fd = g_mkstemp (tmp_path);
  if (tmp_fd < 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not create a download file in %s: %s", tools_dir,
                   g_strerror (saved_errno));
      g_clear_pointer (&tmp_path, g_free);
      goto out;
    }
  close (tmp_fd);

  gchar *size_str = text_format_size (rel->size);
  gchar *start_msg = g_strdup_printf ("Downloading GE-Proton %s (%s)…", rel->tag, size_str);
  job_progress (job, 0.0, start_msg);
  g_free (start_msg);
  g_free (size_str);

  volatile gboolean downloading = TRUE;
  PollData *pd = g_new (PollData, 1);
  pd->job = job;
  pd->tmp_path = g_strdup (tmp_path);
  pd->total_size = rel->size;
  pd->downloading = &downloading;
  GThread *poller = g_thread_new ("proton-dl-poll", poll_thread_func, pd);

  /* -sS: no progress meter (progress comes from polling the file size),
   * but still report errors. */
  gchar *dl_argv[] = { "curl", "-fsSL", "--retry", "3", "-o", tmp_path, rel->url, NULL };
  GError *dl_error = NULL;
  gboolean downloaded = run_capture (job, dl_argv, NULL, &dl_error);

  downloading = FALSE;
  g_thread_join (poller);

  if (!downloaded)
    {
      if (!job->cancelled)
        g_prefix_error (&dl_error, "Download failed: ");
      g_propagate_error (error, dl_error);
      goto out;
    }

  /* Never install a build that can't be checked: GE-Proton publishes a
   * checksum with every release, so a missing one means something is off. */
  if (!rel->sha512_url)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GE-Proton %s has no published checksum, so the download can't be checked. "
                   "It was not installed.", rel->tag);
      goto out;
    }
  job_progress (job, -1, "Checking that the download is intact…");
  gchar *archive_name = g_strconcat (rel->dir_name, ".tar.gz", NULL);
  gboolean verified = verify_sha512 (job, tmp_path, rel->sha512_url, archive_name, error);
  g_free (archive_name);
  if (!verified)
    goto out;
  job_log (job, "Checksum matches (SHA-512).");

  gchar *extract_msg = g_strdup_printf ("Extracting %s…", rel->tag);
  job_progress (job, -1, extract_msg);
  g_free (extract_msg);
  /* Unpack into a folder of its own and move only the expected build into
   * place, so an archive with other top-level folders can't overwrite
   * another build, and a half-unpacked one never looks installed. */
  unpack_dir = g_build_filename (tools_dir, ".gog-ge-proton-unpack-XXXXXX", NULL);
  if (!g_mkdtemp (unpack_dir))
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not create a folder to unpack into in %s: %s", tools_dir,
                   g_strerror (saved_errno));
      g_clear_pointer (&unpack_dir, g_free);
      goto out;
    }
  gchar *tar_argv[] = { "tar", "-xf", tmp_path, "-C", unpack_dir, NULL };
  gchar *tar_output = NULL;
  gboolean extracted = run_capture (job, tar_argv, &tar_output, error);
  if (tar_output && *tar_output)
    job_log (job, tar_output);
  g_free (tar_output);
  if (!extracted)
    goto out;

  gchar *unpacked = g_build_filename (unpack_dir, rel->dir_name, NULL);
  gchar *proton_bin = g_build_filename (unpacked, "proton", NULL);
  gboolean found = g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR);
  g_free (proton_bin);
  if (!found)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "The %s archive did not unpack to %s as expected", rel->tag, rel->dir_name);
      g_free (unpacked);
      goto out;
    }
  /* A broken leftover from an earlier attempt makes way. */
  if (g_file_test (install_dir, G_FILE_TEST_EXISTS))
    remove_tree (install_dir, NULL);
  if (g_rename (unpacked, install_dir) != 0)
    {
      int saved_errno = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved_errno),
                   "Could not move %s into place: %s", rel->dir_name, g_strerror (saved_errno));
      g_free (unpacked);
      goto out;
    }
  g_free (unpacked);

  ok = TRUE;
  job_finished (job, TRUE, rel->tag);

out:
  if (unpack_dir)
    {
      remove_tree (unpack_dir, NULL);
      g_free (unpack_dir);
    }
  if (tmp_path)
    g_remove (tmp_path);
  g_free (tmp_path);
  g_free (install_dir);
  g_free (tools_dir);
  release_info_free (rel);
  return ok;
}

static gpointer
fetch_thread_func (gpointer data)
{
  GogProtonFetchJob *job = data;

  /* fetch_sync() emits "finished" itself on every success path (it needs
   * to free its locals first); on failure it just returns FALSE and we
   * emit "finished" here, uniformly. */
  GError *error = NULL;
  if (!fetch_sync (job, &error))
    job_finished (job, FALSE, error ? error->message : "GE-Proton install failed");
  g_clear_error (&error);

  g_object_unref (job); /* matches the ref taken in gog_proton_fetch_job_start() */
  return NULL;
}

void
gog_proton_fetch_job_start (GogProtonFetchJob *self)
{
  g_object_ref (self);
  self->thread = g_thread_new ("gog-proton-fetch", fetch_thread_func, self);
}

void
gog_proton_fetch_job_cancel (GogProtonFetchJob *self)
{
  self->cancelled = TRUE;
  g_mutex_lock (&self->proc_mutex);
  if (self->current_proc)
    g_subprocess_force_exit (self->current_proc);
  g_mutex_unlock (&self->proc_mutex);
}
