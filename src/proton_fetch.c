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

/* Runs argv to completion, capturing combined output. Registers the
 * subprocess as job->current_proc while it runs so gog_proton_fetch_job_cancel()
 * can force-exit it. */
static gboolean
run_capture (GogProtonFetchJob *job, gchar **argv, gchar **out_output, GError **error)
{
  if (job->cancelled)
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

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = proc;
  /* A cancel that landed between the check above and the spawn would
   * otherwise miss this process. */
  if (job->cancelled)
    g_subprocess_force_exit (proc);
  g_mutex_unlock (&job->proc_mutex);

  gchar *output = NULL;
  gboolean ok = g_subprocess_communicate_utf8 (proc, NULL, NULL, &output, NULL, error);

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = NULL;
  g_mutex_unlock (&job->proc_mutex);

  if (ok && !g_subprocess_get_successful (proc))
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

/* ---- worker thread ---- */

static gboolean
is_safe_dir_name (const char *name)
{
  return name && *name && !strchr (name, '/') && g_strcmp0 (name, ".") != 0 &&
         g_strcmp0 (name, "..") != 0;
}

/* Returns the asset in @release that we can run on this machine, or NULL. */
static json_object *
find_release_asset (json_object *release)
{
  const char *tag = ju_get_string (release, "tag_name", NULL);
  json_object *assets = ju_get_array (release, "assets");
  if (!tag || !assets)
    return NULL;

  gchar *arch_name = ASSET_ARCH ? g_strdup_printf ("%s-%s.tar.gz", tag, ASSET_ARCH) : NULL;
  gchar *plain_name = g_strdup_printf ("%s.tar.gz", tag);
  json_object *arch_asset = NULL, *plain_asset = NULL;

  size_t n = json_object_array_length (assets);
  for (size_t i = 0; i < n; i++)
    {
      json_object *asset = json_object_array_get_idx (assets, i);
      const char *name = ju_get_string (asset, "name", "");
      if (!ju_get_string (asset, "browser_download_url", NULL))
        continue;
      if (arch_name && g_strcmp0 (name, arch_name) == 0)
        arch_asset = asset;
      else if (g_strcmp0 (name, plain_name) == 0)
        plain_asset = asset;
    }

  g_free (arch_name);
  g_free (plain_name);
  return arch_asset ? arch_asset : plain_asset;
}

static gboolean
fetch_sync (GogProtonFetchJob *job, GError **error)
{
  job_progress (job, -1, "Checking for the latest GE-Proton release…");

  gchar *api_argv[] = { "curl", "-fsSL", "-H", "Accept: application/vnd.github+json",
                        "-H", "Cache-Control: no-cache", RELEASES_API_URL, NULL };
  gchar *api_output = NULL;
  if (!run_capture (job, api_argv, &api_output, error))
    return FALSE;

  json_object *root = json_tokener_parse (api_output);
  g_free (api_output);
  if (!root || !json_object_is_type (root, json_type_array))
    {
      if (root)
        json_object_put (root);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Could not parse GitHub's list of GE-Proton releases");
      return FALSE;
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
      return FALSE;
    }

  const char *tag = ju_get_string (best_release, "tag_name", NULL);
  const char *asset_name = ju_get_string (best_asset, "name", "");
  /* The archive unpacks into a directory named after itself, which then
   * sits under the tools dir. */
  gchar *dir_name = g_strndup (asset_name, strlen (asset_name) - strlen (".tar.gz"));
  if (!is_safe_dir_name (tag) || !is_safe_dir_name (dir_name))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GitHub returned an unexpected release name: %s", asset_name);
      g_free (dir_name);
      json_object_put (root);
      return FALSE;
    }
  gchar *tag_dup = g_strdup (tag);
  gchar *log_line = g_strdup_printf ("Newest GE-Proton release: %s", tag_dup);
  job_log (job, log_line);
  g_free (log_line);

  gchar *tools_dir = proton_fetch_get_tools_dir ();
  gchar *install_dir = g_build_filename (tools_dir, dir_name, NULL);
  gchar *proton_bin = g_build_filename (install_dir, "proton", NULL);
  /* Releases fetched before the per-architecture archives live in <tag>/. */
  gchar *legacy_bin = g_build_filename (tools_dir, tag_dup, "proton", NULL);
  g_free (dir_name);
  if (g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR) ||
      g_file_test (legacy_bin, G_FILE_TEST_IS_REGULAR))
    {
      job_log (job, "Already up to date.");
      g_free (legacy_bin);
      g_free (proton_bin);
      g_free (install_dir);
      g_free (tools_dir);
      json_object_put (root);
      job_finished (job, TRUE, tag_dup);
      g_free (tag_dup);
      return TRUE;
    }
  g_free (legacy_bin);
  g_free (proton_bin);

  const char *download_url = ju_get_string (best_asset, "browser_download_url", NULL);
  guint64 total_size = (guint64) ju_get_int64 (best_asset, "size", 0);

  gchar *download_url_dup = g_strdup (download_url);
  json_object_put (root);

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
      g_free (tmp_path);
      g_free (download_url_dup);
      g_free (install_dir);
      g_free (tools_dir);
      g_free (tag_dup);
      return FALSE;
    }
  close (tmp_fd);

  gchar *size_str = text_format_size (total_size);
  gchar *start_msg = g_strdup_printf ("Downloading GE-Proton %s (%s)…", tag_dup, size_str);
  job_progress (job, 0.0, start_msg);
  g_free (start_msg);
  g_free (size_str);

  volatile gboolean downloading = TRUE;
  PollData *pd = g_new (PollData, 1);
  pd->job = job;
  pd->tmp_path = g_strdup (tmp_path);
  pd->total_size = total_size;
  pd->downloading = &downloading;
  GThread *poller = g_thread_new ("proton-dl-poll", poll_thread_func, pd);

  /* -sS: no progress meter (progress comes from polling the file size),
   * but still report errors. */
  gchar *dl_argv[] = { "curl", "-fsSL", "--retry", "3", "-o", tmp_path, download_url_dup, NULL };
  GError *dl_error = NULL;
  gboolean ok = run_capture (job, dl_argv, NULL, &dl_error);

  downloading = FALSE;
  g_thread_join (poller);
  g_free (download_url_dup);

  if (!ok)
    {
      g_remove (tmp_path);
      g_free (tmp_path);
      g_free (install_dir);
      g_free (tools_dir);
      g_free (tag_dup);
      if (error)
        *error = dl_error;
      else
        g_clear_error (&dl_error);
      return FALSE;
    }

  gchar *extract_msg = g_strdup_printf ("Extracting %s…", tag_dup);
  job_progress (job, -1, extract_msg);
  g_free (extract_msg);
  g_mkdir_with_parents (install_dir, 0755);
  gchar *tar_argv[] = { "tar", "-xf", tmp_path, "-C", tools_dir, NULL };
  GError *tar_error = NULL;
  gchar *tar_output = NULL;
  ok = run_capture (job, tar_argv, &tar_output, &tar_error);
  if (tar_output && *tar_output)
    job_log (job, tar_output);
  g_free (tar_output);

  g_remove (tmp_path);
  g_free (tmp_path);

  if (ok)
    {
      proton_bin = g_build_filename (install_dir, "proton", NULL);
      if (!g_file_test (proton_bin, G_FILE_TEST_IS_REGULAR))
        {
          ok = FALSE;
          g_set_error (&tar_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "The %s archive did not unpack to %s as expected", tag_dup, install_dir);
          g_rmdir (install_dir); /* only succeeds if we left it empty */
        }
      g_free (proton_bin);
    }
  g_free (install_dir);
  g_free (tools_dir);

  if (!ok)
    {
      g_free (tag_dup);
      if (error)
        *error = tar_error;
      else
        g_clear_error (&tar_error);
      return FALSE;
    }

  job_finished (job, TRUE, tag_dup);
  g_free (tag_dup);
  return TRUE;
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
