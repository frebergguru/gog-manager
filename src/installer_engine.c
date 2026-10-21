/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "config.h"
#include "installer_engine.h"
#include <glib/gstdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>

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
  pid_t current_pgid;            /* its process group, 0 if none; proc_mutex */
  volatile gboolean cancelled;
  gboolean freetype_explained;   /* install thread only */
  gboolean installer_running;    /* install thread only */
  GCond resume_cond;             /* with proc_mutex: wakes a paused install */
  gboolean resumed;              /* proc_mutex */
};

enum
{
  SIG_PROGRESS,
  SIG_LOG_LINE,
  SIG_FINISHED,
  SIG_FONTS_MISSING,
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
  g_cond_clear (&self->resume_cond);
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
  signals[SIG_FONTS_MISSING] = g_signal_new ("fonts-missing", G_TYPE_FROM_CLASS (klass),
                                              G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                              G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
gog_install_job_init (GogInstallJob *self)
{
  g_mutex_init (&self->proc_mutex);
  g_cond_init (&self->resume_cond);
}

GogInstallJob *
gog_install_job_new (GogGame *game, GogInstallerInfo *installer, const char *install_dir,
                      GogRunnerInfo *runner)
{
  GogInstallJob *self = g_object_new (GOG_TYPE_INSTALL_JOB, NULL);
  self->game = g_object_ref (game);
  self->installer = installer;

  GogGame *base = gog_game_get_base_game (game);
  if (base && gog_game_get_installed (base) && gog_game_get_install_path (base))
    self->install_dir = g_strdup (gog_game_get_install_path (base));
  else
    self->install_dir = g_strdup (install_dir);

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

typedef struct
{
  GogInstallJob *job;
  gboolean paused;
} FontsMsg;

static gboolean
emit_fonts_missing_idle (gpointer data)
{
  FontsMsg *fm = data;
  g_signal_emit (fm->job, signals[SIG_FONTS_MISSING], 0, fm->paused);
  g_object_unref (fm->job);
  g_free (fm);
  return G_SOURCE_REMOVE;
}

static void
job_fonts_missing (GogInstallJob *job, gboolean paused)
{
  FontsMsg *fm = g_new (FontsMsg, 1);
  fm->job = g_object_ref (job);
  fm->paused = paused;
  g_idle_add (emit_fonts_missing_idle, fm);
}

/* Holds the install thread until gog_install_job_resume() or a cancel.
 * Returns FALSE if cancelled. */
static gboolean
job_wait_for_resume (GogInstallJob *job)
{
  g_mutex_lock (&job->proc_mutex);
  while (!job->resumed && !job->cancelled)
    g_cond_wait (&job->resume_cond, &job->proc_mutex);
  gboolean go_on = !job->cancelled;
  g_mutex_unlock (&job->proc_mutex);
  return go_on;
}

/* ---- subprocess execution with live log streaming ---- */

/* Each child leads its own process group, so cancelling can kill whatever
 * it started too: a grandchild still holding the output pipe would keep
 * the read loop below waiting forever. */
static void
child_new_process_group (gpointer user_data)
{
  (void) user_data;
  setpgid (0, 0);
}

/* Stops every Wine process in the job's prefix. The installer keeps running
 * after its wine/proton launcher is killed, and its processes hold the
 * output pipe open. When wait is FALSE (UI thread) this does not block. */
static void
kill_prefix_processes (GogInstallJob *job, gboolean wait)
{
  if (!job->runner || !job->runner->bin_path)
    return;
  GogCommand *kill_cmd = runner_build_kill_command (job->runner, job->install_dir);
  GSpawnFlags flags = G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL;
  if (wait)
    g_spawn_sync (NULL, kill_cmd->argv, kill_cmd->envp, flags, NULL, NULL, NULL, NULL, NULL, NULL);
  else
    g_spawn_async (NULL, kill_cmd->argv, kill_cmd->envp, flags, NULL, NULL, NULL, NULL);
  gog_command_free (kill_cmd);
}

/* Runs argv, streaming its combined output to the log. Exit statuses from 0
 * up to max_ok_status count as success (unzip, for instance, exits 1 for
 * mere warnings such as the shell-script stub in front of GOG's Linux
 * installers). */
static gboolean
run_and_stream_full (GogInstallJob *job, gchar **argv, gchar **envp, gint max_ok_status,
                     GError **error)
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
  g_subprocess_launcher_set_child_setup (launcher, child_new_process_group, NULL, NULL);
  /* Not the AppImage's own folder; see gog_config_launch_dir(). */
  if (gog_config_launch_dir ())
    g_subprocess_launcher_set_cwd (launcher, gog_config_launch_dir ());

  GSubprocess *proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, error);
  g_object_unref (launcher);
  if (!proc)
    return FALSE;

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = proc;
  const char *pid_str = g_subprocess_get_identifier (proc);
  job->current_pgid = pid_str ? (pid_t) g_ascii_strtoll (pid_str, NULL, 10) : 0;
  /* A cancel that landed between the check above and the spawn would
   * otherwise miss this process. */
  if (job->cancelled)
    {
      if (job->current_pgid > 0)
        kill (-job->current_pgid, SIGKILL);
      g_subprocess_force_exit (proc);
    }
  g_mutex_unlock (&job->proc_mutex);

  GInputStream *raw_out = g_subprocess_get_stdout_pipe (proc);
  GDataInputStream *dis = g_data_input_stream_new (raw_out);

  while (TRUE)
    {
      gsize len = 0;
      GError *line_error = NULL;
      /* Not read_line_utf8(): Wine and installers happily print text in
       * legacy code pages, and bailing out on the first invalid byte would
       * stop draining the pipe and stall the child once it fills up. */
      gchar *line = g_data_input_stream_read_line (dis, &len, NULL, &line_error);
      if (line_error)
        {
          g_clear_error (&line_error);
          break;
        }
      if (!line)
        break;
      gchar *valid = g_utf8_make_valid (line, (gssize) len);
      /* Wine repeats its terse FreeType warning for every process; say
       * once, plainly, what is missing and how to install it. */
      if (runner_is_freetype_warning (valid))
        {
          if (!job->freetype_explained)
            {
              job->freetype_explained = TRUE;
              /* Seen while the prefix was being set up, the install pauses
               * for it below; later, the installer is already running. */
              if (job->installer_running)
                job_fonts_missing (job, FALSE);
              gchar **help = runner_freetype_help ();
              for (gchar **h = help; *h; h++)
                job_log (job, *h);
              g_strfreev (help);
            }
        }
      else
        job_log (job, valid);
      g_free (valid);
      g_free (line);
    }

  g_object_unref (dis);

  gboolean waited = g_subprocess_wait (proc, NULL, error);

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = NULL;
  job->current_pgid = 0;
  g_mutex_unlock (&job->proc_mutex);

  if (!waited)
    {
      g_object_unref (proc);
      return FALSE;
    }

  gboolean ok = g_subprocess_get_successful (proc) ||
                (max_ok_status > 0 && g_subprocess_get_if_exited (proc) &&
                 g_subprocess_get_exit_status (proc) <= max_ok_status);
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

static gboolean
run_and_stream (GogInstallJob *job, gchar **argv, gchar **envp, GError **error)
{
  return run_and_stream_full (job, argv, envp, 0, error);
}

/* ---- recursive move (source -> dest, across filesystems if needed) ---- */

static gboolean
move_path (const char *src, const char *dst, GError **error)
{
  if (g_rename (src, dst) == 0)
    return TRUE;

  /* g_rename fails across filesystems (EXDEV) -- fall back to copy+delete.
   * A symlink to a directory is copied as a link, not walked into. */
  if (g_file_test (src, G_FILE_TEST_IS_DIR) && !g_file_test (src, G_FILE_TEST_IS_SYMLINK))
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
              g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                           "Could not remove %s after moving it: %s", src, g_strerror (errno));
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

  if (!g_file_test (job->installer->file_path, G_FILE_TEST_IS_REGULAR))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                   "Installer file not found or is not a regular file: %s", job->installer->file_path);
      return FALSE;
    }

  /* Extract next to the install directory rather than into $TMPDIR: games
   * are often many GB (too big for a tmpfs /tmp), and staying on the same
   * filesystem lets the final move be a cheap rename instead of a copy. */
  gchar *parent = g_path_get_dirname (job->install_dir);
  if (g_mkdir_with_parents (parent, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create %s", parent);
      g_free (parent);
      return FALSE;
    }
  gchar *tmpl = g_build_filename (parent, ".gog-extract-XXXXXX", NULL);
  g_free (parent);
  gchar *tmp_dir = g_mkdtemp (tmpl); /* modifies tmpl in place */
  if (!tmp_dir)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno), "Could not create temp dir");
      g_free (tmpl);
      return FALSE;
    }

  gchar *argv[] = { "unzip", "-qq", job->installer->file_path, "-d", tmp_dir, NULL };
  /* GOG's .sh installers are a shell stub followed by a zip, so unzip warns
   * about "extra bytes at beginning" and exits 1 even though it worked. */
  gboolean ok = run_and_stream_full (job, argv, NULL, 1, error);
  /* Cancel pressed as unzip finished: nothing has been moved in yet. */
  if (ok && job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ok = FALSE;
    }
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
  /* When the whole temp dir was moved in, it brought its 0700 mode. */
  if (ok)
    g_chmod (job->install_dir, 0755);
  /* A cancel that lands now comes too late: the files are in place, and
   * failing would leave them without an install record to uninstall them
   * by. Only the post-install script is skipped. */

  gchar *postinst = g_build_filename (job->install_dir, "support", "postinst.sh", NULL);
  if (ok && !job->cancelled && g_file_test (postinst, G_FILE_TEST_IS_REGULAR))
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
      /* Cancel stopped the script: the install isn't finished. */
      if (job->cancelled)
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          ok = FALSE;
        }
    }
  g_free (postinst);

  remove_dir_recursive (tmp_dir);
  g_free (tmpl);
  return ok;
}

/* ---- Windows: watching the installer while it runs ---- */

/* A Windows installer under Wine says nothing on stdout, so without this
 * the dialog shows a pulsing bar for many minutes and a stuck installer
 * looks exactly like a busy one. The watcher thread copies the installer's
 * own log (/LOG=c:\install.log) into the dialog and measures how much has
 * been written into the install directory. */

#define WATCH_INTERVAL_US (G_USEC_PER_SEC)
#define STALL_SECONDS 120

typedef struct
{
  GogInstallJob *job;
  gchar *log_path;          /* <wine root>/drive_c/install.log */
  goffset log_pos;
  GString *partial;         /* log text after the last newline */
  guint64 expected;         /* installer + its .bin parts, 0 if unknown */
  guint64 baseline;         /* already in the install dir (DLC, reinstall) */
  volatile gint stop;
  GThread *thread;
} InstallWatch;

/* Bytes under dir, leaving out the Wine/Proton prefix that lives there. */
static guint64
dir_size_without_prefix (const char *dir, gboolean top)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  if (!d)
    return 0;
  guint64 total = 0;
  const char *name;
  while ((name = g_dir_read_name (d)))
    {
      if (top && (strcmp (name, "proton-prefix") == 0 || strcmp (name, "prefix") == 0))
        continue;
      gchar *path = g_build_filename (dir, name, NULL);
      GStatBuf st;
      if (g_lstat (path, &st) == 0)
        {
          if (S_ISDIR (st.st_mode))
            total += dir_size_without_prefix (path, FALSE);
          else if (S_ISREG (st.st_mode))
            total += (guint64) st.st_size;
        }
      g_free (path);
    }
  g_dir_close (d);
  return total;
}

/* The installer .exe plus its setup_<name>-1.bin, -2.bin ... parts. The
 * installed size is not known up front, but GOG games are mostly already
 * compressed media, so this is a fair estimate of it. */
static guint64
installer_set_size (const char *exe_path)
{
  gchar *dir = g_path_get_dirname (exe_path);
  gchar *base = g_path_get_basename (exe_path);
  gchar *dot = strrchr (base, '.');
  if (dot)
    *dot = '\0';
  gchar *prefix = g_strconcat (base, "-", NULL);

  guint64 total = 0;
  GDir *d = g_dir_open (dir, 0, NULL);
  if (d)
    {
      const char *name;
      while ((name = g_dir_read_name (d)))
        {
          gboolean part = g_str_has_prefix (name, prefix) && g_str_has_suffix (name, ".bin");
          gchar *path = g_build_filename (dir, name, NULL);
          GStatBuf st;
          if ((part || strcmp (path, exe_path) == 0) && g_stat (path, &st) == 0)
            total += (guint64) st.st_size;
          g_free (path);
        }
      g_dir_close (d);
    }
  g_free (prefix);
  g_free (base);
  g_free (dir);
  return total;
}

/* Copies whole new lines of the installer's log into the job log. Returns
 * TRUE if anything new was read. */
static gboolean
watch_read_log (InstallWatch *w)
{
  GStatBuf st;
  if (g_stat (w->log_path, &st) != 0)
    return FALSE;
  /* A second run (the non-silent retry) starts the log over. */
  if (st.st_size < w->log_pos)
    {
      w->log_pos = 0;
      g_string_truncate (w->partial, 0);
    }
  if (st.st_size == w->log_pos)
    return FALSE;

  FILE *f = g_fopen (w->log_path, "rb");
  if (!f)
    return FALSE;
  gboolean got = FALSE;
  if (fseeko (f, w->log_pos, SEEK_SET) == 0)
    {
      char buf[8192];
      size_t n;
      while ((n = fread (buf, 1, sizeof buf, f)) > 0)
        {
          w->log_pos += (goffset) n;
          g_string_append_len (w->partial, buf, (gssize) n);
          got = TRUE;
        }
    }
  fclose (f);

  char *nl;
  while ((nl = memchr (w->partial->str, '\n', w->partial->len)))
    {
      gsize len = (gsize) (nl - w->partial->str);
      if (len > 0 && w->partial->str[len - 1] == '\r')
        len--;
      if (len > 0)
        {
          /* Inno Setup writes its log in the Windows code page. */
          gchar *valid = g_utf8_make_valid (w->partial->str, (gssize) len);
          job_log (w->job, valid);
          g_free (valid);
        }
      g_string_erase (w->partial, 0, (gssize) (nl - w->partial->str) + 1);
    }
  return got;
}

static gpointer
install_watch_thread (gpointer data)
{
  InstallWatch *w = data;
  guint64 last_written = 0;
  gint64 last_change = g_get_monotonic_time ();
  gboolean stall_logged = FALSE;

  while (!g_atomic_int_get (&w->stop))
    {
      g_usleep (WATCH_INTERVAL_US);
      if (g_atomic_int_get (&w->stop))
        break;

      gboolean log_moved = watch_read_log (w);
      guint64 size = dir_size_without_prefix (w->job->install_dir, TRUE);
      guint64 written = size > w->baseline ? size - w->baseline : 0;
      gint64 now = g_get_monotonic_time ();
      if (log_moved || written != last_written)
        {
          last_change = now;
          last_written = written;
          stall_logged = FALSE;
        }
      gint64 idle = (now - last_change) / G_USEC_PER_SEC;

      gchar *done = g_format_size (written);
      gchar *msg;
      double fraction = -1;
      if (idle >= STALL_SECONDS)
        {
          msg = g_strdup_printf ("No progress for %" G_GINT64_FORMAT " min, may be stuck",
                                 idle / 60);
          if (!stall_logged)
            {
              job_log (w->job, "The installer has written no files and logged nothing for 2 "
                               "minutes. It may be stuck, or waiting for a window that did "
                               "not appear. Cancel stops it.");
              stall_logged = TRUE;
            }
        }
      else if (written == 0)
        msg = g_strdup ("Starting the installer… (no game files written yet)");
      else if (w->expected > 0)
        {
          gchar *total = g_format_size (w->expected);
          msg = g_strdup_printf ("Installing… %s of about %s", done, total);
          g_free (total);
          fraction = MIN ((double) written / (double) w->expected, 0.99);
        }
      else
        msg = g_strdup_printf ("Installing… %s written", done);

      job_progress (w->job, fraction, msg);
      g_free (msg);
      g_free (done);
    }

  watch_read_log (w); /* the last lines, written as it finished */
  return NULL;
}

static InstallWatch *
install_watch_start (GogInstallJob *job)
{
  InstallWatch *w = g_new0 (InstallWatch, 1);
  w->job = g_object_ref (job);
  gchar *wine_root = runner_wine_root_dir (job->runner->kind, job->install_dir);
  w->log_path = g_build_filename (wine_root, "drive_c", "install.log", NULL);
  g_free (wine_root);
  /* A log left by an earlier run would be overwritten from the start, and
   * whatever the new run wrote before the first look would be missed:
   * start without one. */
  g_remove (w->log_path);
  w->partial = g_string_new (NULL);
  w->expected = installer_set_size (job->installer->file_path);
  /* A DLC goes into its game's folder: count only what this install adds. */
  w->baseline = dir_size_without_prefix (job->install_dir, TRUE);
  w->thread = g_thread_new ("gog-install-watch", install_watch_thread, w);
  return w;
}

static void
install_watch_stop (InstallWatch *w)
{
  g_atomic_int_set (&w->stop, 1);
  g_thread_join (w->thread);
  g_object_unref (w->job);
  g_free (w->log_path);
  g_string_free (w->partial, TRUE);
  g_free (w);
}

/* ---- Windows: Wine/Proton silent install with fallback ---- */

static gboolean
install_windows_sync (GogInstallJob *job, GError **error)
{
  if (!job->runner || !job->runner->bin_path)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "No Wine or Proton runner selected; install Wine or download GE-Proton first");
      return FALSE;
    }

  if (g_mkdir_with_parents (job->install_dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create install directory %s", job->install_dir);
      return FALSE;
    }

  if (!runner_ensure_prefix_dir (job->runner->kind, job->install_dir, error))
    return FALSE;

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

  /* Wine said while setting up the prefix that it has no fonts; without
   * them GOG's installer can hang. Wait for the user to fix it or cancel. */
  if (job->freetype_explained)
    {
      job_progress (job, -1, "Paused: fonts are missing for Windows programs");
      job_fonts_missing (job, TRUE);
      if (!job_wait_for_resume (job))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
          return FALSE;
        }
    }

  gchar *wine_root = runner_wine_root_dir (job->runner->kind, job->install_dir);
  gboolean linked = runner_link_c_game (wine_root, job->install_dir, error);
  g_free (wine_root);
  if (!linked)
    return FALSE;

  job_progress (job, -1, "Running installer (silent)…");
  job->installer_running = TRUE;
  InstallWatch *watch = install_watch_start (job);
  GogCommand *silent = runner_build_install_command (job->runner, job->installer->file_path,
                                                      job->install_dir,
                                                      job->installer->language_code, TRUE);
  GError *silent_err = NULL;
  gboolean ok = run_and_stream (job, silent->argv, silent->envp, &silent_err);
  gog_command_free (silent);

  if (ok)
    {
      install_watch_stop (watch);
      return TRUE;
    }

  if (job->cancelled)
    {
      install_watch_stop (watch);
      if (error)
        *error = g_steal_pointer (&silent_err);
      else
        g_clear_error (&silent_err);
      return FALSE;
    }

  job_log (job, "Silent install failed, retrying with the installer's own window…");
  g_clear_error (&silent_err);
  /* The retry writes its own log from the start. */
  install_watch_stop (watch);
  watch = install_watch_start (job);

  GogCommand *interactive = runner_build_install_command (job->runner, job->installer->file_path,
                                                            job->install_dir,
                                                            job->installer->language_code, FALSE);
  ok = run_and_stream (job, interactive->argv, interactive->envp, error);
  gog_command_free (interactive);
  install_watch_stop (watch);
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

  /* Catch anything the cancel's own wineserver -k raced past. */
  if (job->cancelled)
    kill_prefix_processes (job, TRUE);

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
  g_mutex_lock (&self->proc_mutex);
  self->cancelled = TRUE;
  g_cond_broadcast (&self->resume_cond);
  if (self->current_pgid > 0)
    kill (-self->current_pgid, SIGKILL);
  if (self->current_proc)
    g_subprocess_force_exit (self->current_proc);
  g_mutex_unlock (&self->proc_mutex);
  kill_prefix_processes (self, FALSE);
}

void
gog_install_job_resume (GogInstallJob *self)
{
  g_mutex_lock (&self->proc_mutex);
  self->resumed = TRUE;
  g_cond_broadcast (&self->resume_cond);
  g_mutex_unlock (&self->proc_mutex);
}

/* Adds dir to keep both as written and with symlinks resolved, since real
 * is resolved (e.g. /home -> /var/home on Fedora Atomic). */
static void
add_protected (GPtrArray *keep, const char *dir)
{
  g_ptr_array_add (keep, g_canonicalize_filename (dir, NULL));
  char *resolved = realpath (dir, NULL);
  if (resolved)
    {
      g_ptr_array_add (keep, g_strdup (resolved));
      free (resolved);
    }
}

/* TRUE for folders an uninstall must never delete, whatever the install
 * record says: /, the home folder and anything holding it, the standard
 * user folders (Downloads, Documents...), a mount point, the folder with
 * the game's own installers (or one holding it), the install root (or one
 * holding it), and a folder holding another recorded game's install (a game
 * once installed straight into an empty ~/Games, with others later put in
 * ~/Games/<slug>). real is resolved. */
static gboolean
is_protected_dir (const char *real, GogGame *game)
{
  gchar *real_slash = g_strconcat (real, G_DIR_SEPARATOR_S, NULL);
  gboolean protected = g_strcmp0 (real, "/") == 0;

  GPtrArray *keep = g_ptr_array_new_with_free_func (g_free);
  add_protected (keep, g_get_home_dir ());
  for (int d = 0; d < G_USER_N_DIRECTORIES; d++)
    {
      const char *dir = g_get_user_special_dir (d);
      if (dir)
        add_protected (keep, dir);
    }
  const char *installers = gog_game_get_folder_path (game);
  if (installers && *installers)
    add_protected (keep, installers);
  GogConfig *config = gog_config_load ();
  if (config->install_root && *config->install_root)
    add_protected (keep, config->install_root);
  gog_config_free (config);
  for (guint i = 0; i < keep->len && !protected; i++)
    {
      const char *k = g_ptr_array_index (keep, i);
      /* The folder itself, or one it lies inside. */
      protected = g_strcmp0 (real, k) == 0 || g_str_has_prefix (k, real_slash);
    }
  g_ptr_array_unref (keep);

  /* Only strictly inside: a DLC's record shares its base game's folder. */
  if (!protected)
    {
      GogState *state = gog_state_load ();
      GStrv paths = gog_state_dup_install_paths (state);
      for (GStrv p = paths; *p && !protected; p++)
        {
          char *other = realpath (*p, NULL);
          gchar *canon = other ? g_strdup (other) : g_canonicalize_filename (*p, NULL);
          free (other);
          protected = g_str_has_prefix (canon, real_slash);
          g_free (canon);
        }
      g_strfreev (paths);
      gog_state_free (state);
    }

  /* A mount point: the top of a drive is no game's folder. */
  if (!protected)
    {
      gchar *parent = g_path_get_dirname (real);
      GStatBuf a, b;
      protected = g_stat (real, &a) == 0 && g_stat (parent, &b) == 0 && a.st_dev != b.st_dev;
      g_free (parent);
    }
  g_free (real_slash);
  return protected;
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

  /* A DLC is installed into its base game's directory, and its files can't
   * be told apart from the base game's -- deleting that directory would
   * take the base game with it. Only forget the DLC's install record. */
  GogGame *base = gog_game_get_base_game (game);
  gboolean shares_base_dir = base && g_strcmp0 (gog_game_get_install_path (base), path) == 0;

  /* The path comes from the state file, or a folder the user picked; judge
   * it by where it really is (a symlinked game folder is its target). */
  char *resolved = realpath (path, NULL);
  gchar *real = resolved ? g_strdup (resolved) : g_canonicalize_filename (path, NULL);
  free (resolved);
  if (!shares_base_dir && (!g_path_is_absolute (path) || is_protected_dir (real, game)))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_PERM,
                   "Refusing to delete %s: not a game install directory", path);
      g_free (real);
      return FALSE;
    }

  if (!shares_base_dir && g_file_test (real, G_FILE_TEST_IS_DIR))
    {
      if (!remove_dir_recursive (real))
        {
          g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                       "Could not fully remove %s", real);
          g_free (real);
          return FALSE;
        }
      /* The link itself, when the game folder was one. */
      if (g_file_test (path, G_FILE_TEST_IS_SYMLINK))
        g_remove (path);
    }
  g_free (real);

  gog_game_set_installed (game, FALSE);
  gog_game_set_install_path (game, NULL);
  gog_game_set_runner_kind (game, GOG_RUNNER_NONE);
  gog_game_set_runner_path (game, NULL);
  gog_game_set_prefix_path (game, NULL);
  return TRUE;
}

guint64
gog_install_estimate_bytes (guint64 installer_size, gboolean new_prefix)
{
  return installer_size * 2 + (new_prefix ? (guint64) 1 << 30 : 0);
}

gboolean
gog_free_space (const char *path, guint64 *free_bytes)
{
  if (!path || !g_path_is_absolute (path))
    return FALSE;
  gchar *dir = g_strdup (path);
  while (!g_file_test (dir, G_FILE_TEST_IS_DIR))
    {
      gchar *up = g_path_get_dirname (dir);
      gboolean top = strcmp (up, dir) == 0;
      g_free (dir);
      dir = up;
      if (top)
        break;
    }
  GFile *file = g_file_new_for_path (dir);
  g_free (dir);
  GFileInfo *info = g_file_query_filesystem_info (file, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, NULL,
                                                  NULL);
  g_object_unref (file);
  gboolean known = info && g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
  if (known)
    *free_bytes = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
  g_clear_object (&info);
  return known;
}
