#include "installer_engine.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>

struct _GogInstallJob
{
  GObject parent_instance;

  GogGame *game;                 /* owned (reffed) */
  GogInstallerInfo *installer;   /* borrowed, owned by game */
  gchar *install_dir;
  GogRunnerInfo *runner;         /* owned copy, NULL for native */

  GThread *thread;
  GMutex proc_mutex;
  GSubprocess *current_proc;     /* protected by proc_mutex */
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

G_DEFINE_FINAL_TYPE (GogInstallJob, gog_install_job, G_TYPE_OBJECT)

static void
gog_install_job_finalize (GObject *object)
{
  GogInstallJob *self = GOG_INSTALL_JOB (object);
  g_clear_object (&self->game);
  g_free (self->install_dir);
  gog_runner_info_free (self->runner);
  g_mutex_clear (&self->proc_mutex);
  if (self->thread)
    g_thread_unref (self->thread);
  G_OBJECT_CLASS (gog_install_job_parent_class)->finalize (object);
}

static void
gog_install_job_class_init (GogInstallJobClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_install_job_finalize;

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
gog_install_job_init (GogInstallJob *self)
{
  g_mutex_init (&self->proc_mutex);
}

GogInstallJob *
gog_install_job_new (GogGame *game, GogInstallerInfo *installer, const char *install_root,
                      GogRunnerInfo *runner)
{
  GogInstallJob *self = g_object_new (GOG_TYPE_INSTALL_JOB, NULL);
  self->game = g_object_ref (game);
  self->installer = installer;

  GogGame *base = gog_game_get_base_game (game);
  if (base && gog_game_get_installed (base) && gog_game_get_install_path (base))
    self->install_dir = g_strdup (gog_game_get_install_path (base));
  else
    self->install_dir = g_build_filename (install_root, gog_game_get_slug (game), NULL);

  if (runner)
    self->runner = gog_runner_info_new (runner->kind, runner->name, runner->bin_path);

  return self;
}

const char *
gog_install_job_get_install_dir (GogInstallJob *self)
{
  return self->install_dir;
}

/* ---- marshalling signals back to the main thread ---- */

typedef struct
{
  GogInstallJob *job; /* reffed */
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
job_progress (GogInstallJob *job, double fraction, const char *message)
{
  ProgressMsg *pm = g_new (ProgressMsg, 1);
  pm->job = g_object_ref (job);
  pm->fraction = fraction;
  pm->message = g_strdup (message);
  g_idle_add (emit_progress_idle, pm);
}

typedef struct
{
  GogInstallJob *job;
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
job_log (GogInstallJob *job, const char *line)
{
  LogMsg *lm = g_new (LogMsg, 1);
  lm->job = g_object_ref (job);
  lm->line = g_strdup (line);
  g_idle_add (emit_log_idle, lm);
}

typedef struct
{
  GogInstallJob *job;
  gboolean success;
  gchar *error_message;
} FinishedMsg;

static gboolean
emit_finished_idle (gpointer data)
{
  FinishedMsg *fm = data;
  g_signal_emit (fm->job, signals[SIG_FINISHED], 0, fm->success, fm->error_message);
  g_free (fm->error_message);
  g_object_unref (fm->job);
  g_free (fm);
  return G_SOURCE_REMOVE;
}

static void
job_finished (GogInstallJob *job, gboolean success, const char *error_message)
{
  FinishedMsg *fm = g_new (FinishedMsg, 1);
  fm->job = g_object_ref (job);
  fm->success = success;
  fm->error_message = g_strdup (error_message);
  g_idle_add (emit_finished_idle, fm);
}

/* ---- subprocess execution with live log streaming ---- */

static gboolean
run_and_stream (GogInstallJob *job, gchar **argv, gchar **envp, GError **error)
{
  if (job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }

  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                              G_SUBPROCESS_FLAGS_STDERR_MERGE);
  if (envp)
    g_subprocess_launcher_set_environ (launcher, envp);

  GSubprocess *proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, error);
  g_object_unref (launcher);
  if (!proc)
    return FALSE;

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = proc;
  g_mutex_unlock (&job->proc_mutex);

  GInputStream *raw_out = g_subprocess_get_stdout_pipe (proc);
  GDataInputStream *dis = g_data_input_stream_new (raw_out);

  while (TRUE)
    {
      gsize len = 0;
      GError *line_error = NULL;
      gchar *line = g_data_input_stream_read_line_utf8 (dis, &len, NULL, &line_error);
      if (line_error)
        {
          g_clear_error (&line_error);
          break;
        }
      if (!line)
        break;
      job_log (job, line);
      g_free (line);
    }

  g_object_unref (dis);

  gboolean waited = g_subprocess_wait (proc, NULL, error);

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = NULL;
  g_mutex_unlock (&job->proc_mutex);

  if (!waited)
    {
      g_object_unref (proc);
      return FALSE;
    }

  gboolean ok = g_subprocess_get_successful (proc);
  if (!ok && error && !*error)
    {
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
  g_object_unref (proc);
  return ok;
}

/* ---- recursive move (source -> dest, across filesystems if needed) ---- */

static gboolean
move_path (const char *src, const char *dst, GError **error)
{
  if (g_rename (src, dst) == 0)
    return TRUE;

  /* g_rename fails across filesystems (EXDEV) -- fall back to copy+delete. */
  if (g_file_test (src, G_FILE_TEST_IS_DIR))
    {
      if (g_mkdir_with_parents (dst, 0755) != 0)
        {
          g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "mkdir %s", dst);
          return FALSE;
        }
      GDir *d = g_dir_open (src, 0, error);
      if (!d)
        return FALSE;
      const char *name;
      gboolean ok = TRUE;
      while (ok && (name = g_dir_read_name (d)))
        {
          gchar *s = g_build_filename (src, name, NULL);
          gchar *dd = g_build_filename (dst, name, NULL);
          ok = move_path (s, dd, error);
          g_free (s);
          g_free (dd);
        }
      g_dir_close (d);
      if (ok)
        {
          if (g_rmdir (src) != 0)
            {
              g_warning ("Failed to remove directory %s: %s", src, g_strerror (errno));
              ok = FALSE;
            }
        }
      return ok;
    }
  else
    {
      GFile *sf = g_file_new_for_path (src);
      GFile *df = g_file_new_for_path (dst);
      gboolean ok = g_file_copy (sf, df, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, error);
      if (ok)
        {
          GError *del_error = NULL;
          if (!g_file_delete (sf, NULL, &del_error))
            {
              g_warning ("Failed to delete %s after move: %s", src, del_error->message);
              g_error_free (del_error);
            }
        }
      g_object_unref (sf);
      g_object_unref (df);
      return ok;
    }
}

static gboolean
remove_dir_recursive (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (!d)
    return g_remove (path) == 0;
  const char *name;
  gboolean ok = TRUE;
  while ((name = g_dir_read_name (d)))
    {
      gchar *child = g_build_filename (path, name, NULL);
      if (g_file_test (child, G_FILE_TEST_IS_DIR) && !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
        {
          if (!remove_dir_recursive (child))
            ok = FALSE;
        }
      else
        {
          if (g_remove (child) != 0)
            ok = FALSE;
        }
      g_free (child);
    }
  g_dir_close (d);
  if (g_rmdir (path) != 0)
    ok = FALSE;
  return ok;
}

/* ---- Linux native: unzip extraction ---- */

static gboolean
extract_linux_sync (GogInstallJob *job, GError **error)
{
  job_progress (job, -1, "Extracting…");

  gchar *tmpl = g_build_filename (g_get_tmp_dir (), "gog-extract-XXXXXX", NULL);
  gchar *tmp_dir = g_mkdtemp (tmpl); /* modifies tmpl in place */
  if (!tmp_dir)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "Could not create temp dir");
      g_free (tmpl);
      return FALSE;
    }

  if (!g_file_test (job->installer->file_path, G_FILE_TEST_IS_REGULAR))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                   "Installer file not found or is not a regular file: %s", job->installer->file_path);
      remove_dir_recursive (tmp_dir);
      g_free (tmpl);
      return FALSE;
    }

  gchar *argv[] = { "unzip", "-qq", job->installer->file_path, "-d", tmp_dir, NULL };
  gboolean ok = run_and_stream (job, argv, NULL, error);
  if (!ok)
    {
      remove_dir_recursive (tmp_dir);
      g_free (tmpl);
      return FALSE;
    }

  gchar *source = g_build_filename (tmp_dir, "data", "noarch", NULL);
  if (!g_file_test (source, G_FILE_TEST_IS_DIR))
    {
      g_free (source);
      source = g_strdup (tmp_dir);
    }

  job_progress (job, -1, "Copying game files…");
  if (g_mkdir_with_parents (job->install_dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create install directory %s", job->install_dir);
      ok = FALSE;
    }
  else
    ok = move_path (source, job->install_dir, error);
  g_free (source);

  gchar *postinst = g_build_filename (job->install_dir, "support", "postinst.sh", NULL);
  if (ok && g_file_test (postinst, G_FILE_TEST_IS_REGULAR))
    {
      if (g_chmod (postinst, 0755) != 0)
        {
          g_warning ("Failed to set execute permission on %s", postinst);
        }
      gchar *pargv[] = { postinst, NULL };
      GError *post_err = NULL;
      if (!run_and_stream (job, pargv, NULL, &post_err))
        {
          job_log (job, "Post-install script reported an error (continuing):");
          if (post_err)
            {
              job_log (job, post_err->message);
              g_clear_error (&post_err);
            }
        }
    }
  g_free (postinst);

  remove_dir_recursive (tmp_dir);
  g_free (tmpl);
  return ok;
}

/* ---- Windows: Wine/Proton silent install with fallback ---- */

static gboolean
install_windows_sync (GogInstallJob *job, GError **error)
{
  if (g_mkdir_with_parents (job->install_dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create install directory %s", job->install_dir);
      return FALSE;
    }

  job_progress (job, -1, "Preparing Wine/Proton prefix…");
  GogCommand *boot = runner_build_bootstrap_command (job->runner, job->install_dir);
  GError *boot_err = NULL;
  if (!run_and_stream (job, boot->argv, boot->envp, &boot_err))
    {
      job_log (job, "Prefix bootstrap reported a non-zero exit (continuing):");
      if (boot_err)
        {
          job_log (job, boot_err->message);
          g_clear_error (&boot_err);
        }
    }
  gog_command_free (boot);

  if (job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }

  gchar *wine_root = runner_wine_root_dir (job->runner->kind, job->install_dir);
  gboolean linked = runner_link_c_game (wine_root, job->install_dir, error);
  g_free (wine_root);
  if (!linked)
    return FALSE;

  job_progress (job, -1, "Running installer (silent)…");
  GogCommand *silent = runner_build_install_command (job->runner, job->installer->file_path,
                                                      job->install_dir,
                                                      job->installer->language_code, TRUE);
  GError *silent_err = NULL;
  gboolean ok = run_and_stream (job, silent->argv, silent->envp, &silent_err);
  gog_command_free (silent);

  if (ok)
    return TRUE;

  if (job->cancelled)
    {
      if (error)
        *error = g_steal_pointer (&silent_err);
      else
        g_clear_error (&silent_err);
      return FALSE;
    }

  job_log (job, "Silent install failed, retrying with the installer's own window…");
  g_clear_error (&silent_err);

  GogCommand *interactive = runner_build_install_command (job->runner, job->installer->file_path,
                                                            job->install_dir,
                                                            job->installer->language_code, FALSE);
  ok = run_and_stream (job, interactive->argv, interactive->envp, error);
  gog_command_free (interactive);
  return ok;
}

/* ---- thread entry point ---- */

static gpointer
install_thread_func (gpointer data)
{
  GogInstallJob *job = data;
  GError *error = NULL;
  gboolean ok;

  if (job->installer->os == GOG_OS_LINUX)
    ok = extract_linux_sync (job, &error);
  else
    ok = install_windows_sync (job, &error);

  if (ok)
    job_progress (job, 1.0, "Done");

  job_finished (job, ok, ok ? NULL : (error ? error->message : "Install failed"));
  g_clear_error (&error);
  g_object_unref (job); /* matches the ref taken in gog_install_job_start() */
  return NULL;
}

void
gog_install_job_start (GogInstallJob *self)
{
  g_object_ref (self); /* released at the end of install_thread_func */
  self->thread = g_thread_new ("gog-install", install_thread_func, self);
}

void
gog_install_job_cancel (GogInstallJob *self)
{
  self->cancelled = TRUE;
  g_mutex_lock (&self->proc_mutex);
  if (self->current_proc)
    g_subprocess_force_exit (self->current_proc);
  g_mutex_unlock (&self->proc_mutex);
}

gboolean
gog_uninstall_game (GogGame *game, GError **error)
{
  const char *path = gog_game_get_install_path (game);
  if (!path || !*path)
    {
      gog_game_set_installed (game, FALSE);
      return TRUE;
    }

  if (g_file_test (path, G_FILE_TEST_IS_DIR))
    {
      if (!remove_dir_recursive (path))
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "Could not fully remove %s", path);
          return FALSE;
        }
    }

  gog_game_set_installed (game, FALSE);
  gog_game_set_install_path (game, NULL);
  gog_game_set_runner_kind (game, GOG_RUNNER_NONE);
  gog_game_set_runner_path (game, NULL);
  gog_game_set_prefix_path (game, NULL);
  return TRUE;
}
