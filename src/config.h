/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_CONFIG_H
#define GOG_CONFIG_H

#include <glib.h>
#include "game.h"

G_BEGIN_DECLS

typedef struct
{
  gchar *library_path;      /* root folder scanned for games, e.g. .../GOG/Games */
  gchar *install_root;      /* where new installs go by default */
  GogRunnerKind default_runner_kind;
  gchar *default_runner_path; /* wine binary or a proton dir, matching the kind above */
  GStrv custom_runner_paths;  /* user-added wine binaries / proton dirs */
  gchar *library_view;        /* "grid" (default), "list" or "details" */
} GogConfig;

/* Loads <app dir>/.gog-manager-configs/gog-manager.cfg. Returns a zero-valued
 * (all-NULL/defaults) config, never NULL, if the file doesn't exist yet or
 * fails to parse -- first-run and corrupt-file are handled the same way by
 * the caller (prompt for a library path). */
GogConfig *gog_config_load (void);

/* A file in the app's config folder, .gog-manager-configs in the
 * AppImage's folder (or the executable's), which is created if needed.
 * Transfer full. */
gchar *gog_config_app_file (const char *name);

/* The folder GOG Manager was started from. The AppImage starts it in its
 * own usr folder (WebKit finds its helpers from there) and passes the
 * original in GOG_MANAGER_LAUNCH_DIR; programs GOG Manager runs get that
 * one, as they would without the AppImage. NULL when not set: then the
 * current folder is it. */
const char *gog_config_launch_dir (void);
gboolean   gog_config_save (GogConfig *config, GError **error);
void       gog_config_free (GogConfig *config);

/* The library root to actually scan: the manually-configured library_path if
 * the user has set one, otherwise the current working directory (so dropping
 * the executable into a GOG/Games folder and running it there Just Works).
 * Never returns NULL; caller owns the returned string. */
gchar *gog_config_effective_library_path (GogConfig *config);

/* TRUE when the effective path comes from the current working directory rather
 * than an explicit setting (i.e. library_path is unset). */
gboolean gog_config_library_path_is_default (GogConfig *config);

/* Per-game install state, keyed by product id. Stored separately from
 * gog-manager.cfg (in `<app dir>/.gog-manager-configs/gog-manager-state.json`) so that switching
 * or re-scanning the library path never loses install records. */
typedef struct _GogState GogState;

GogState *gog_state_load (void);
gboolean  gog_state_save (GogState *state, GError **error);
void      gog_state_free (GogState *state);

/* Copies any recorded install state for game's id onto the (freshly
 * scanned) game object. */
void gog_state_apply_to_game (GogState *state, GogGame *game);

/* Records game's current install state (as set via the gog_game_set_*
 * mutators) into state, ready for gog_state_save(). */
void gog_state_update_from_game (GogState *state, GogGame *game);

/* Removes any recorded state for a product id (used on uninstall). */
void gog_state_remove (GogState *state, gint64 id);

G_END_DECLS

#endif
