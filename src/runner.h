/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_RUNNER_H
#define GOG_RUNNER_H

#include <glib.h>
#include "game.h"
#include "config.h"

G_BEGIN_DECLS

typedef struct
{
  GogRunnerKind kind;
  gchar *name;      /* display name, e.g. "GE-Proton9-20" or "System Wine" */
  gchar *bin_path;  /* wine: path to the wine binary. proton: path to the
                        proton *directory* (containing a "proton" script). */
} GogRunnerInfo;

GogRunnerInfo *gog_runner_info_new (GogRunnerKind kind, const char *name, const char *bin_path);
void           gog_runner_info_free (GogRunnerInfo *info);

/* Every usable runner found on this system: system wine (if on PATH),
 * every Proton build under Heroic's tool dir / Steam's compatibilitytools.d
 * locations, plus config's custom_runner_paths. Element type:
 * GogRunnerInfo*, transfer full (g_ptr_array_unref frees everything). */
GPtrArray *runner_discover_all (GogConfig *config);

/* A ready-to-spawn command: argv[0] is the program, argv is NULL-terminated
 * (as required by GSubprocessLauncher/g_spawn); envp is a full environment
 * (base process environment plus any overrides), also NULL-terminated. */
typedef struct
{
  gchar **argv;
  gchar **envp;
} GogCommand;

void gog_command_free (GogCommand *cmd);

/* Where a Wine/Proton prefix for a given game install directory lives.
 * For Proton, this is STEAM_COMPAT_DATA_PATH -- Proton itself keeps the
 * actual Wine prefix at "<this>/pfx". */
gchar *runner_prefix_dir_for_install (GogRunnerKind kind, const char *install_dir);

/* The actual Wine-prefix root to hand to runner_link_c_game(): the prefix
 * dir itself for Wine, or "<prefix>/pfx" for Proton. */
gchar *runner_wine_root_dir (GogRunnerKind kind, const char *install_dir);

/* Creates prefix's dosdevices/c:/game as a relative symlink pointing back
 * at install_dir, so files an installer writes to "c:\game" land directly
 * in install_dir (outside the prefix, portable). No-op if it already
 * exists. wine_root is the prefix dir for Wine, or "<prefix>/pfx" for
 * Proton. */
gboolean runner_link_c_game (const char *wine_root, const char *install_dir, GError **error);

/* Builds `wine wineboot -u` (or the Proton equivalent). Wine/Proton only
 * create their prefix skeleton -- including dosdevices/c:, which
 * runner_link_c_game() needs to already exist -- the first time some wine
 * command actually runs against it. Run and wait for this once, before
 * calling runner_link_c_game(), when initializing a fresh prefix. */
GogCommand *runner_build_bootstrap_command (GogRunnerInfo *runner, const char *install_dir);

/* Builds `wine <installer> /DIR=c:\game /LANG=... [silent switches]` (or
 * the Proton equivalent), with WINEPREFIX / STEAM_COMPAT_* set in envp.
 * When silent is FALSE, the unattended switches are omitted so the
 * installer's own UI appears (fallback for installers that reject
 * unattended mode). Transfer full. */
GogCommand *runner_build_install_command (GogRunnerInfo *runner, const char *installer_path,
                                           const char *install_dir, const char *lang_code,
                                           gboolean silent);

/* Builds the command to run an already-installed Windows game's exe
 * (relative to install_dir, Windows-style backslashes are handled
 * internally) via `wine start /b /wait /d c:\game\<workdir> c:\game\<path>
 * <arguments...>`. */
GogCommand *runner_build_launch_command (GogRunnerInfo *runner, const char *install_dir,
                                          const char *exe_relpath, const char *working_dir_relpath,
                                          const char *arguments);

/* winecfg / regedit / winetricks helpers on a game's existing prefix
 * (parity with common Linux GOG tools; convenience-only). */
GogCommand *runner_build_winecfg_command (GogRunnerInfo *runner, const char *install_dir);
GogCommand *runner_build_regedit_command (GogRunnerInfo *runner, const char *install_dir);

G_END_DECLS

#endif
