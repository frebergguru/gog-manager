/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_GAME_H
#define GOG_GAME_H

#include <glib-object.h>
#include <gio/gio.h>
#include "gamesdb.h"

G_BEGIN_DECLS

#define GOG_TYPE_GAME (gog_game_get_type ())
G_DECLARE_FINAL_TYPE (GogGame, gog_game, GOG, GAME, GObject)

typedef enum
{
  GOG_OS_WINDOWS,
  GOG_OS_LINUX,
  GOG_OS_MAC,
} GogOs;

typedef enum
{
  GOG_PRODUCT_GAME,
  GOG_PRODUCT_DLC,
} GogProductType;

typedef enum
{
  GOG_RUNNER_NONE,
  GOG_RUNNER_NATIVE,
  GOG_RUNNER_WINE,
  GOG_RUNNER_PROTON,
} GogRunnerKind;

/* One installer file actually found on disk for a game, cross-referenced
 * against the product json's downloads.installers[] entries. */
typedef struct
{
  GogOs   os;
  gchar  *language_code;   /* e.g. "en", may be NULL */
  gchar  *language_full;   /* e.g. "English" */
  gchar  *version;
  gchar  *file_path;       /* absolute path to the main installer file */
  guint64 size;
} GogInstallerInfo;

GogInstallerInfo *gog_installer_info_new (void);
void               gog_installer_info_free (GogInstallerInfo *info);

/* A bonus-content file under extras/. */
typedef struct
{
  gchar  *name;
  gchar  *type;
  gchar  *file_path;
  guint64 size;
} GogExtraInfo;

GogExtraInfo *gog_extra_info_new (void);
void          gog_extra_info_free (GogExtraInfo *info);

GogGame *gog_game_new (void);

/* Static metadata, set once during scan. */
gint64        gog_game_get_id (GogGame *self);
void          gog_game_set_id (GogGame *self, gint64 id);
const char   *gog_game_get_slug (GogGame *self);
void          gog_game_set_slug (GogGame *self, const char *slug);
const char   *gog_game_get_title (GogGame *self);
void          gog_game_set_title (GogGame *self, const char *title);
const char   *gog_game_get_folder_path (GogGame *self);
void          gog_game_set_folder_path (GogGame *self, const char *path);
const char   *gog_game_get_icon_path (GogGame *self);
void          gog_game_set_icon_path (GogGame *self, const char *path);
const char   *gog_game_get_logo_path (GogGame *self);
void          gog_game_set_logo_path (GogGame *self, const char *path);
const char   *gog_game_get_description (GogGame *self);
void          gog_game_set_description (GogGame *self, const char *text);
/* Contents of the product folder's serials.txt (CD keys), or NULL. */
const char   *gog_game_get_serials (GogGame *self);
void          gog_game_set_serials (GogGame *self, const char *text);

gboolean gog_game_get_supports_os (GogGame *self, GogOs os);
void     gog_game_set_supports_os (GogGame *self, GogOs os, gboolean supported);

/* NULL-terminated array of human-readable language names, owned by self. */
const char *const *gog_game_get_languages (GogGame *self);
void                gog_game_set_languages (GogGame *self, GStrv languages);

/* GOG's genres for the game, main one first (e.g. "Role-playing",
 * "Action", "Fantasy"), from the categories cache; NULL if not known.
 * NULL-terminated, owned by self; set takes ownership and emits
 * notify::categories when they changed. */
const char *const *gog_game_get_categories (GogGame *self);
void                gog_game_set_categories (GogGame *self, GStrv categories);

/* What GOG's games database says about the game (artwork, summary,
 * developers, themes...), from the gamesdb cache; NULL if not known.
 * Owned by self; set takes ownership and emits notify::gamesdb. */
const GogGamesdbInfo *gog_game_get_gamesdb (GogGame *self);
void                  gog_game_set_gamesdb (GogGame *self, GogGamesdbInfo *info);

/* When GOG first released the game (the product json's release_date), as
 * Unix time; 0 if not known. */
gint64 gog_game_get_release_date (GogGame *self);
void   gog_game_set_release_date (GogGame *self, gint64 unix_time);

GogProductType gog_game_get_product_type (GogGame *self);
void           gog_game_set_product_type (GogGame *self, GogProductType type);

/* ids from the product json's related_products[], used to link a DLC
 * product to its base game. Owned by self. */
const gint64 *gog_game_get_related_ids (GogGame *self, guint *n);
void          gog_game_set_related_ids (GogGame *self, const gint64 *ids, guint n);

/* Installers found on disk. Element type: GogInstallerInfo*, owned by self. */
GPtrArray *gog_game_get_installers (GogGame *self);
void       gog_game_add_installer (GogGame *self, GogInstallerInfo *info);

/* Distinct languages of the installers found on disk, sorted by name: the
 * native names (e.g. "Deutsch") or, with codes, GOG's language codes in the
 * same order (e.g. "de"). Transfer full; never NULL. */
GStrv gog_game_dup_installer_languages (GogGame *self);
GStrv gog_game_dup_installer_language_codes (GogGame *self);

/* Extras found on disk. Element type: GogExtraInfo*, owned by self. */
GPtrArray *gog_game_get_extras (GogGame *self);
void       gog_game_add_extra (GogGame *self, GogExtraInfo *info);

guint64 gog_game_get_total_size (GogGame *self);

/* Mutable install state (persisted separately in state.json). Changing
 * "installed" emits notify::installed so the UI can refresh live. */
gboolean gog_game_get_installed (GogGame *self);
void     gog_game_set_installed (GogGame *self, gboolean installed);
const char *gog_game_get_install_path (GogGame *self);
void        gog_game_set_install_path (GogGame *self, const char *path);
GogRunnerKind gog_game_get_runner_kind (GogGame *self);
void          gog_game_set_runner_kind (GogGame *self, GogRunnerKind kind);
const char *gog_game_get_runner_path (GogGame *self);
void        gog_game_set_runner_path (GogGame *self, const char *path);
const char *gog_game_get_prefix_path (GogGame *self);
void        gog_game_set_prefix_path (GogGame *self, const char *path);

/* Base game this DLC belongs to, resolved after a full library scan.
 * NULL for base games or unresolved DLC. Not a reference (library owns
 * the lifetime; both live in the same GListStore for as long as either
 * does). */
GogGame *gog_game_get_base_game (GogGame *self);
void     gog_game_set_base_game (GogGame *self, GogGame *base);

const char *gog_os_to_string (GogOs os);
const char *gog_runner_kind_to_string (GogRunnerKind kind);
GogRunnerKind gog_runner_kind_from_string (const char *str);

G_END_DECLS

#endif
