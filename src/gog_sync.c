/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "gog_sync.h"
#include "config.h"
#include "json_util.h"
#include <stdio.h>
#include <string.h>

#define LGOG "lgogdownloader"
#define RESULTS_FILE "gog-manager-updates.json"

/* ---- items ---- */

void
gog_sync_item_free (GogSyncItem *item)
{
  if (!item)
    return;
  g_free (item->gamename);
  g_free (item);
}

gboolean
gog_sync_is_available (void)
{
  gchar *path = g_find_program_in_path (LGOG);
  gboolean found = path != NULL;
  g_free (path);
  return found;
}

gchar *
gog_sync_pretty_name (const char *gamename)
{
  gchar *out = g_strdup (gamename);
  gboolean word_start = TRUE;
  for (gchar *p = out; *p; p++)
    {
      if (*p == '_')
        {
          *p = ' ';
          word_start = TRUE;
        }
      else
        {
          if (word_start)
            *p = g_ascii_toupper (*p);
          word_start = FALSE;
        }
    }
  return out;
}

static gboolean
game_folder_exists (const char *library_dir, const char *gamename)
{
  gchar *path = g_build_filename (library_dir ? library_dir : ".", gamename, NULL);
  gboolean exists = g_file_test (path, G_FILE_TEST_IS_DIR);
  g_free (path);
  return exists;
}

static gint
compare_items (gconstpointer a, gconstpointer b)
{
  const GogSyncItem *ia = *(const GogSyncItem *const *) a;
  const GogSyncItem *ib = *(const GogSyncItem *const *) b;
  return g_strcmp0 (ia->gamename, ib->gamename);
}

GPtrArray *
gog_sync_parse_status (const char *output, const char *library_dir,
                       const char *const *base_names)
{
  gchar *current_game = NULL;
  GHashTable *by_name = g_hash_table_new (g_str_hash, g_str_equal);
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);

  gchar **lines = g_strsplit (output ? output : "", "\n", -1);
  for (gchar **line = lines; *line; line++)
    {
      gchar **tok = g_strsplit (g_strstrip (*line), " ", 3);
      gboolean is_status = g_strv_length (tok) == 3 &&
                           (g_strcmp0 (tok[0], "OK") == 0 || g_strcmp0 (tok[0], "ND") == 0 ||
                            g_strcmp0 (tok[0], "FS") == 0 || g_strcmp0 (tok[0], "MD5") == 0);
      const char *game = NULL;
      if (is_status)
        {
          if (!base_names || g_strv_contains (base_names, tok[1]) || !current_game)
            {
              g_free (current_game);
              current_game = g_strdup (tok[1]);
            }
          game = current_game; /* a DLC name counts under its game */
        }
      gboolean wanted = g_strv_length (tok) == 3 &&
                        (g_strcmp0 (tok[0], "ND") == 0 || g_strcmp0 (tok[0], "FS") == 0 ||
                         g_strcmp0 (tok[0], "MD5") == 0);
      if (wanted)
        {
          GogSyncItem *item = g_hash_table_lookup (by_name, game);
          if (!item)
            {
              item = g_new0 (GogSyncItem, 1);
              item->gamename = g_strdup (game);
              item->kind = game_folder_exists (library_dir, game) ? GOG_SYNC_UPDATE
                                                                  : GOG_SYNC_NEW_GAME;
              g_ptr_array_add (items, item);
              g_hash_table_insert (by_name, item->gamename, item);
            }
          item->n_files++;
        }
      g_strfreev (tok);
    }
  g_strfreev (lines);

  g_hash_table_unref (by_name);
  g_free (current_game);
  g_ptr_array_sort (items, compare_items);
  return items;
}

/* ---- remembered results ---- */

GPtrArray *
gog_sync_load_results (gint64 *out_checked_unix)
{
  GPtrArray *items = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_sync_item_free);
  if (out_checked_unix)
    *out_checked_unix = 0;
  gchar *path = gog_config_app_file (RESULTS_FILE);
  json_object *root = ju_load_file (path, NULL);
  g_free (path);
  if (!root)
    return items;

  if (out_checked_unix)
    *out_checked_unix = ju_get_int64 (root, "checked", 0);
  json_object *arr = ju_get_array (root, "items");
  size_t n = arr ? json_object_array_length (arr) : 0;
  for (size_t i = 0; i < n; i++)
    {
      json_object *rec = json_object_array_get_idx (arr, i);
      const char *name = ju_get_string (rec, "gamename", NULL);
      if (!name)
        continue;
      GogSyncItem *item = g_new0 (GogSyncItem, 1);
      item->gamename = g_strdup (name);
      item->kind = g_strcmp0 (ju_get_string (rec, "kind", ""), "new") == 0 ? GOG_SYNC_NEW_GAME
                                                                          : GOG_SYNC_UPDATE;
      item->n_files = (guint) ju_get_int64 (rec, "files", 0);
      g_ptr_array_add (items, item);
    }
  json_object_put (root);
  return items;
}

void
gog_sync_save_results (GPtrArray *items)
{
  json_object *root = json_object_new_object ();
  json_object_object_add (root, "checked", json_object_new_int64 (g_get_real_time () / G_USEC_PER_SEC));
  json_object *arr = json_object_new_array ();
  for (guint i = 0; items && i < items->len; i++)
    {
      GogSyncItem *item = g_ptr_array_index (items, i);
      json_object *rec = json_object_new_object ();
      json_object_object_add (rec, "gamename", json_object_new_string (item->gamename));
      json_object_object_add (rec, "kind",
                              json_object_new_string (item->kind == GOG_SYNC_NEW_GAME ? "new"
                                                                                       : "update"));
      json_object_object_add (rec, "files", json_object_new_int64 (item->n_files));
      json_object_array_add (arr, rec);
    }
  json_object_object_add (root, "items", arr);

  gchar *path = gog_config_app_file (RESULTS_FILE);
  GError *error = NULL;
  if (!g_file_set_contents (path, json_object_to_json_string_ext (root, JSON_C_TO_STRING_PRETTY),
                            -1, &error))
    {
      g_warning ("Could not save GOG update results: %s", error->message);
      g_clear_error (&error);
    }
  g_free (path);
  json_object_put (root);
}

/* ---- the job ---- */

typedef enum
{
  JOB_CHECK,
  JOB_DOWNLOAD,
} JobKind;

struct _GogSyncJob
{
  GObject parent_instance;

  JobKind kind;
  gchar *library_dir;
  GStrv gamenames;    /* download only */
  GPtrArray *items;   /* check result */

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

G_DEFINE_FINAL_TYPE (GogSyncJob, gog_sync_job, G_TYPE_OBJECT)

static void
gog_sync_job_finalize (GObject *object)
{
  GogSyncJob *self = GOG_SYNC_JOB (object);
  g_mutex_clear (&self->proc_mutex);
  if (self->thread)
    g_thread_unref (self->thread);
  g_free (self->library_dir);
  g_strfreev (self->gamenames);
  if (self->items)
    g_ptr_array_unref (self->items);
  G_OBJECT_CLASS (gog_sync_job_parent_class)->finalize (object);
}

static void
gog_sync_job_class_init (GogSyncJobClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_sync_job_finalize;

  signals[SIG_PROGRESS] = g_signal_new ("progress", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_DOUBLE,
                                        G_TYPE_STRING);
  signals[SIG_LOG_LINE] = g_signal_new ("log-line", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_FINISHED] = g_signal_new ("finished", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                                        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_BOOLEAN,
                                        G_TYPE_STRING);
}

static void
gog_sync_job_init (GogSyncJob *self)
{
  g_mutex_init (&self->proc_mutex);
}

GogSyncJob *
gog_sync_job_new_check (const char *library_dir)
{
  GogSyncJob *self = g_object_new (GOG_TYPE_SYNC_JOB, NULL);
  self->kind = JOB_CHECK;
  self->library_dir = g_strdup (library_dir);
  return self;
}

GogSyncJob *
gog_sync_job_new_download (const char *library_dir, const char *const *gamenames)
{
  GogSyncJob *self = g_object_new (GOG_TYPE_SYNC_JOB, NULL);
  self->kind = JOB_DOWNLOAD;
  self->library_dir = g_strdup (library_dir);
  self->gamenames = g_strdupv ((gchar **) gamenames);
  return self;
}

GPtrArray *
gog_sync_job_get_items (GogSyncJob *self)
{
  return self->items;
}

/* ---- marshalling signals back to the main thread (same pattern as
 * proton_fetch.c) ---- */

typedef struct
{
  GogSyncJob *job;
  guint signal;
  double fraction;
  gboolean success;
  gchar *text;
} SignalMsg;

static gboolean
emit_idle (gpointer data)
{
  SignalMsg *m = data;
  if (m->signal == SIG_PROGRESS)
    g_signal_emit (m->job, signals[SIG_PROGRESS], 0, m->fraction, m->text);
  else if (m->signal == SIG_LOG_LINE)
    g_signal_emit (m->job, signals[SIG_LOG_LINE], 0, m->text);
  else
    g_signal_emit (m->job, signals[SIG_FINISHED], 0, m->success, m->text);
  g_free (m->text);
  g_object_unref (m->job);
  g_free (m);
  return G_SOURCE_REMOVE;
}

static void
queue_signal (GogSyncJob *job, guint signal, double fraction, gboolean success, const char *text)
{
  SignalMsg *m = g_new0 (SignalMsg, 1);
  m->job = g_object_ref (job);
  m->signal = signal;
  m->fraction = fraction;
  m->success = success;
  m->text = g_strdup (text);
  g_idle_add (emit_idle, m);
}

#define job_progress(job, fraction, msg) queue_signal (job, SIG_PROGRESS, fraction, FALSE, msg)
#define job_log(job, line) queue_signal (job, SIG_LOG_LINE, 0, FALSE, line)

/* ---- running lgogdownloader ---- */

/* Called for each output line, ANSI escapes removed. by_cr is TRUE when the
 * line ended in a carriage return, i.e. a progress display that will be
 * overwritten. */
typedef void (*LineFunc) (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data);

static gchar *
strip_ansi (const char *s)
{
  GString *out = g_string_new (NULL);
  for (const char *p = s; *p; p++)
    {
      if (*p == '\033')
        {
          if (p[1] == '[')
            {
              p += 2;
              while (*p && !g_ascii_isalpha (*p))
                p++;
              if (!*p)
                break;
            }
          else if (p[1] == ']')
            {
              /* OSC (e.g. the terminal progress "ESC ] 9;4;..."), ends in BEL or ESC \ */
              p += 2;
              while (*p && *p != '\a' && !(*p == '\033' && p[1] == '\\'))
                p++;
              if (!*p)
                break;
              if (*p == '\033')
                p++;
            }
          continue;
        }
      g_string_append_c (out, *p);
    }
  return g_string_free (out, FALSE);
}

static void
flush_line (GogSyncJob *job, GString *buf, gboolean by_cr, LineFunc on_line, gpointer data)
{
  gchar *clean = strip_ansi (buf->str);
  g_strstrip (clean);
  if (*clean)
    on_line (job, clean, by_cr, data);
  g_free (clean);
  g_string_truncate (buf, 0);
}

static gboolean
run_streaming (GogSyncJob *job, gchar **argv, LineFunc on_line, gpointer data, GError **error)
{
  if (job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      return FALSE;
    }

  GSubprocessLauncher *launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                                             G_SUBPROCESS_FLAGS_STDERR_MERGE);
  /* Plain output: no colours or progress escapes beyond what we strip. */
  g_subprocess_launcher_setenv (launcher, "TERM", "dumb", TRUE);
  GSubprocess *proc = g_subprocess_launcher_spawnv (launcher, (const gchar *const *) argv, error);
  g_object_unref (launcher);
  if (!proc)
    return FALSE;

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = proc;
  if (job->cancelled)
    g_subprocess_force_exit (proc);
  g_mutex_unlock (&job->proc_mutex);

  GInputStream *out = g_subprocess_get_stdout_pipe (proc);
  GString *buf = g_string_new (NULL);
  gchar chunk[4096];
  gssize n;
  while ((n = g_input_stream_read (out, chunk, sizeof chunk, NULL, NULL)) > 0)
    for (gssize i = 0; i < n; i++)
      {
        if (chunk[i] == '\n' || chunk[i] == '\r')
          flush_line (job, buf, chunk[i] == '\r', on_line, data);
        else
          g_string_append_c (buf, chunk[i]);
      }
  flush_line (job, buf, FALSE, on_line, data);
  g_string_free (buf, TRUE);

  gboolean ok = g_subprocess_wait (proc, NULL, error);

  g_mutex_lock (&job->proc_mutex);
  job->current_proc = NULL;
  g_mutex_unlock (&job->proc_mutex);

  if (ok && job->cancelled)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
      ok = FALSE;
    }
  else if (ok && !g_subprocess_get_successful (proc))
    {
      ok = FALSE;
      if (g_subprocess_get_if_signaled (proc))
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "lgogdownloader was stopped (signal %d)",
                     g_subprocess_get_term_sig (proc));
      else
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED,
                     "lgogdownloader exited with status %d", g_subprocess_get_exit_status (proc));
    }

  g_object_unref (proc);
  return ok;
}

/* "Getting game info 12 / 395" -> 12, 395 */
static gboolean
parse_counter (const char *line, const char *prefix, guint *done, guint *total)
{
  const char *p = strstr (line, prefix);
  if (!p)
    return FALSE;
  return sscanf (p + strlen (prefix), " %u / %u", done, total) == 2 && *total > 0;
}

static void
collect_line (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data)
{
  GString *collected = data;
  guint done, total;
  if (parse_counter (line, "Getting game info", &done, &total))
    {
      gchar *msg = g_strdup_printf ("Checking game %u of %u…", done, total);
      job_progress (job, (double) done / total, msg);
      g_free (msg);
    }
  else if (parse_counter (line, "Getting game names", &done, &total) ||
           parse_counter (line, "Getting product data", &done, &total))
    job_progress (job, -1, "Getting your list of games from GOG…");
  else if (!by_cr)
    {
      g_string_append (collected, line);
      g_string_append_c (collected, '\n');
    }
}

/* lgogdownloader redraws its download display (per-thread "#0 <file>",
 * "45% ▕███▏ … ETA", "Total: …") with fresh lines, so those go to the
 * progress bar; the log keeps the lasting messages. */
typedef struct
{
  gchar *current_file;
  gint64 last_progress; /* monotonic µs; the display redraws far faster than needed */
} DownloadState;

static void
log_line (GogSyncJob *job, const char *line, gboolean by_cr, gpointer data)
{
  DownloadState *state = data;
  guint done, total;
  if (parse_counter (line, "Getting game info", &done, &total))
    {
      gchar *msg = g_strdup_printf ("Getting game details… %u of %u", done, total);
      job_progress (job, (double) done / total, msg);
      g_free (msg);
      return;
    }
  if (strstr (line, "Getting game names") || strstr (line, "Getting product data"))
    {
      job_progress (job, -1, "Getting your list of games from GOG…");
      return;
    }
  if (line[0] == '#' && g_ascii_isdigit (line[1]))
    {
      const char *space = strchr (line, ' ');
      if (space)
        {
          g_free (state->current_file);
          state->current_file = g_strdup (space + 1);
        }
      return;
    }
  if (g_ascii_isdigit (line[0]))
    {
      char *end = NULL;
      guint64 percent = g_ascii_strtoull (line, &end, 10);
      if (end && *end == '%')
        {
          gint64 now = g_get_monotonic_time ();
          if (now - state->last_progress < 100 * 1000)
            return;
          state->last_progress = now;
          gchar *msg = g_strdup_printf ("Downloading %s… %u%%",
                                        state->current_file ? state->current_file : "",
                                        (guint) MIN (percent, 100));
          job_progress (job, MIN (percent, 100) / 100.0, msg);
          g_free (msg);
          return;
        }
    }
  if (g_str_has_prefix (line, "Total:") || by_cr)
    return;

  const char *complete = strstr (line, "Download complete: ");
  const char *deleting = g_str_has_prefix (line, "Deleting ") ? line + strlen ("Deleting ") : NULL;
  if (complete)
    {
      gchar *msg = g_strdup (complete + strlen ("Download complete: "));
      gchar *speed = strstr (msg, " (@");
      if (speed)
        *speed = '\0';
      gchar *out = g_strdup_printf ("Downloaded %s", msg);
      job_log (job, out);
      g_free (out);
      g_free (msg);
    }
  else if (deleting)
    {
      gchar *name = g_path_get_basename (deleting);
      gchar *out = g_strdup_printf ("Deleted old version: %s", name);
      job_log (job, out);
      g_free (out);
      g_free (name);
    }
  else
    job_log (job, line);
}

static gboolean
check_login (GogSyncJob *job, GError **error)
{
  gchar *argv[] = { LGOG, "--check-login-status", NULL };
  GString *out = g_string_new (NULL);
  gboolean ok = run_streaming (job, argv, collect_line, out, error);
  gboolean logged_in = ok && strstr (out->str, "Login status: Logged in") != NULL;
  g_string_free (out, TRUE);
  if (ok && !logged_in)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           "lgogdownloader is not logged in to GOG. Open a terminal, run "
                           "\"lgogdownloader --login\" and try again.");
      return FALSE;
    }
  return ok;
}

static gboolean
check_sync (GogSyncJob *job, GError **error)
{
  job_progress (job, -1, "Checking the GOG login…");
  if (!check_login (job, error))
    return FALSE;

  job_progress (job, -1, "Getting your list of games from GOG…");
  gchar *list_argv[] = { LGOG, "--list", "games", NULL };
  GString *names_out = g_string_new (NULL);
  if (!run_streaming (job, list_argv, collect_line, names_out, error))
    {
      g_string_free (names_out, TRUE);
      return FALSE;
    }
  gchar **base_names = g_strsplit (g_strstrip (names_out->str), "\n", -1);
  g_string_free (names_out, TRUE);

  gchar *argv[] = { LGOG, "--status", "--size-only", "--exclude", "patches",
                    "--directory", job->library_dir, NULL };
  GString *out = g_string_new (NULL);
  gboolean ok = run_streaming (job, argv, collect_line, out, error);
  if (ok)
    {
      job->items = gog_sync_parse_status (out->str, job->library_dir,
                                          (const char *const *) base_names);
      gog_sync_save_results (job->items);
    }
  g_string_free (out, TRUE);
  g_strfreev (base_names);
  return ok;
}

static gboolean
download_sync (GogSyncJob *job, GError **error)
{
  job_progress (job, -1, "Checking the GOG login…");
  if (!check_login (job, error))
    return FALSE;

  GString *regex = g_string_new ("^(");
  for (gchar **name = job->gamenames; *name; name++)
    {
      gchar *escaped = g_regex_escape_string (*name, -1);
      g_string_append_printf (regex, "%s%s", name == job->gamenames ? "" : "|", escaped);
      g_free (escaped);
    }
  g_string_append (regex, ")$");

  job_progress (job, -1, "Starting the download…");
  /* The options the library is downloaded with. --check-orphans
   * --delete-orphans runs after the download and deletes files GOG no
   * longer offers, i.e. the old versions. */
  gchar *argv[] = { LGOG, "--download", "--threads", "3", "--exclude", "patches",
                    "--check-orphans", "--delete-orphans", "--save-serials", "--save-icon",
                    "--save-logo", "--save-product-json", "--directory", job->library_dir,
                    "--game", regex->str, NULL };
  DownloadState state = { NULL, 0 };
  gboolean ok = run_streaming (job, argv, log_line, &state, error);
  g_free (state.current_file);
  g_string_free (regex, TRUE);

  if (ok)
    {
      /* Those games are now up to date. */
      GPtrArray *saved = gog_sync_load_results (NULL);
      for (guint i = saved->len; i > 0; i--)
        {
          GogSyncItem *item = g_ptr_array_index (saved, i - 1);
          if (g_strv_contains ((const char *const *) job->gamenames, item->gamename))
            g_ptr_array_remove_index (saved, i - 1);
        }
      gog_sync_save_results (saved);
      g_ptr_array_unref (saved);
    }
  return ok;
}

static gpointer
job_thread_func (gpointer data)
{
  GogSyncJob *job = data;
  GError *error = NULL;
  gboolean ok = job->kind == JOB_CHECK ? check_sync (job, &error) : download_sync (job, &error);
  queue_signal (job, SIG_FINISHED, 0, ok,
                ok ? NULL : (error ? error->message : "lgogdownloader failed"));
  g_clear_error (&error);
  g_object_unref (job); /* matches the ref taken in gog_sync_job_start() */
  return NULL;
}

void
gog_sync_job_start (GogSyncJob *self)
{
  g_object_ref (self);
  self->thread = g_thread_new ("gog-sync", job_thread_func, self);
}

void
gog_sync_job_cancel (GogSyncJob *self)
{
  self->cancelled = TRUE;
  g_mutex_lock (&self->proc_mutex);
  if (self->current_proc)
    g_subprocess_force_exit (self->current_proc);
  g_mutex_unlock (&self->proc_mutex);
}
