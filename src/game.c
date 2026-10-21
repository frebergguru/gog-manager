/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include "game.h"
#include <string.h>

struct _GogGame
{
  GObject parent_instance;

  gint64 id;
  gchar *slug;
  gchar *gamename;
  gchar *title;
  gchar *folder_path;
  gchar *icon_path;
  gchar *logo_path;
  gchar *description;
  gchar *serials;

  gboolean supports_os[3];
  GStrv languages;
  GStrv categories;
  GogGamesdbInfo *gamesdb;
  GogStoreInfo *store;
  GogProductInfo *product;
  gint64 release_date;

  GogProductType product_type;
  GArray *related_ids; /* gint64 */

  GPtrArray *installers; /* GogInstallerInfo* */
  GPtrArray *extras;     /* GogExtraInfo* */

  gboolean downloaded;
  gboolean installed;
  gchar *install_path;
  GogRunnerKind runner_kind;
  gchar *runner_path;
  gchar *prefix_path;
  gint64 last_played;  /* unix time, 0 if never */
  gint64 play_seconds;

  GogGame *base_game; /* owned; the base never refs its DLCs, so no cycle */
};

enum
{
  PROP_0,
  PROP_INSTALLED,
  PROP_CATEGORIES,
  PROP_GAMESDB,
  PROP_STORE,
  PROP_PLAYED,
  N_PROPS,
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_FINAL_TYPE (GogGame, gog_game, G_TYPE_OBJECT)

GogInstallerInfo *
gog_installer_info_new (void)
{
  return g_new0 (GogInstallerInfo, 1);
}

void
gog_installer_info_free (GogInstallerInfo *info)
{
  if (!info)
    return;
  g_free (info->language_code);
  g_free (info->language_full);
  g_free (info->version);
  g_free (info->file_path);
  g_free (info);
}

GogExtraInfo *
gog_extra_info_new (void)
{
  return g_new0 (GogExtraInfo, 1);
}

void
gog_extra_info_free (GogExtraInfo *info)
{
  if (!info)
    return;
  g_free (info->name);
  g_free (info->type);
  g_free (info->file_path);
  g_free (info);
}

static void
gog_game_finalize (GObject *object)
{
  GogGame *self = GOG_GAME (object);

  g_clear_object (&self->base_game);
  g_free (self->slug);
  g_free (self->gamename);
  g_free (self->title);
  g_free (self->folder_path);
  g_free (self->icon_path);
  g_free (self->logo_path);
  g_free (self->description);
  g_free (self->serials);
  g_strfreev (self->languages);
  g_strfreev (self->categories);
  gog_gamesdb_info_free (self->gamesdb);
  gog_store_info_free (self->store);
  gog_product_info_free (self->product);
  if (self->related_ids)
    g_array_unref (self->related_ids);
  if (self->installers)
    g_ptr_array_unref (self->installers);
  if (self->extras)
    g_ptr_array_unref (self->extras);
  g_free (self->install_path);
  g_free (self->runner_path);
  g_free (self->prefix_path);

  G_OBJECT_CLASS (gog_game_parent_class)->finalize (object);
}

static void
gog_game_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GogGame *self = GOG_GAME (object);
  switch (prop_id)
    {
    case PROP_INSTALLED:
      g_value_set_boolean (value, self->installed);
      break;
    case PROP_CATEGORIES:
      g_value_set_boxed (value, self->categories);
      break;
    case PROP_GAMESDB:
      g_value_set_boolean (value, self->gamesdb != NULL);
      break;
    case PROP_STORE:
      g_value_set_boolean (value, self->store != NULL);
      break;
    case PROP_PLAYED:
      g_value_set_int64 (value, self->last_played);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gog_game_class_init (GogGameClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gog_game_finalize;
  object_class->get_property = gog_game_get_property;

  properties[PROP_INSTALLED] =
      g_param_spec_boolean ("installed", "Installed", "Whether the game is installed",
                             FALSE, G_PARAM_READABLE);
  properties[PROP_CATEGORIES] =
      g_param_spec_boxed ("categories", "Categories", "GOG's genres for the game",
                          G_TYPE_STRV, G_PARAM_READABLE);
  /* Only notifies; read it with gog_game_get_gamesdb(). */
  properties[PROP_GAMESDB] =
      g_param_spec_boolean ("gamesdb", "Games database", "Whether GOG's games database knows the game",
                            FALSE, G_PARAM_READABLE);
  /* Likewise, with gog_game_get_store_info(). */
  properties[PROP_STORE] =
      g_param_spec_boolean ("store", "Store details", "Whether GOG's store API knows the game",
                            FALSE, G_PARAM_READABLE);
  properties[PROP_PLAYED] =
      g_param_spec_int64 ("played", "Last played", "When the game was last started (unix time)",
                          0, G_MAXINT64, 0, G_PARAM_READABLE);
  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gog_game_init (GogGame *self)
{
  self->installers = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_installer_info_free);
  self->extras = g_ptr_array_new_with_free_func ((GDestroyNotify) gog_extra_info_free);
  self->related_ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  self->product_type = GOG_PRODUCT_GAME;
  self->runner_kind = GOG_RUNNER_NONE;
  self->product = gog_product_info_new ();
  self->downloaded = TRUE;
}

gboolean
gog_game_get_downloaded (GogGame *self)
{
  return self->downloaded;
}

void
gog_game_set_downloaded (GogGame *self, gboolean downloaded)
{
  self->downloaded = downloaded;
}

GogGame *
gog_game_new (void)
{
  return g_object_new (GOG_TYPE_GAME, NULL);
}

gint64
gog_game_get_id (GogGame *self)
{
  return self->id;
}

void
gog_game_set_id (GogGame *self, gint64 id)
{
  self->id = id;
}

const char *
gog_game_get_slug (GogGame *self)
{
  return self->slug;
}

void
gog_game_set_slug (GogGame *self, const char *slug)
{
  g_free (self->slug);
  self->slug = g_strdup (slug);
}

const char *
gog_game_get_gamename (GogGame *self)
{
  return self->gamename;
}

void
gog_game_set_gamename (GogGame *self, const char *gamename)
{
  g_free (self->gamename);
  self->gamename = g_strdup (gamename);
}

gint64
gog_game_local_id (const char *gamename)
{
  /* FNV-1a, folded to 62 bits so the negation can't overflow. */
  guint64 h = 14695981039346656037ULL;
  for (const char *c = gamename ? gamename : ""; *c; c++)
    {
      h ^= (guchar) *c;
      h *= 1099511628211ULL;
    }
  h &= G_GUINT64_CONSTANT (0x3fffffffffffffff);
  return -(gint64) MAX (h, 1);
}

const char *
gog_game_get_title (GogGame *self)
{
  return self->title;
}

void
gog_game_set_title (GogGame *self, const char *title)
{
  g_free (self->title);
  self->title = g_strdup (title);
}

const char *
gog_game_get_folder_path (GogGame *self)
{
  return self->folder_path;
}

void
gog_game_set_folder_path (GogGame *self, const char *path)
{
  g_free (self->folder_path);
  self->folder_path = g_strdup (path);
}

const char *
gog_game_get_icon_path (GogGame *self)
{
  return self->icon_path;
}

void
gog_game_set_icon_path (GogGame *self, const char *path)
{
  g_free (self->icon_path);
  self->icon_path = g_strdup (path);
}

const char *
gog_game_get_logo_path (GogGame *self)
{
  return self->logo_path;
}

void
gog_game_set_logo_path (GogGame *self, const char *path)
{
  g_free (self->logo_path);
  self->logo_path = g_strdup (path);
}

const char *
gog_game_get_description (GogGame *self)
{
  return self->description;
}

void
gog_game_set_description (GogGame *self, const char *text)
{
  g_free (self->description);
  self->description = g_strdup (text);
}

const char *
gog_game_get_serials (GogGame *self)
{
  return self->serials;
}

void
gog_game_set_serials (GogGame *self, const char *text)
{
  g_free (self->serials);
  self->serials = g_strdup (text);
}

gboolean
gog_game_get_supports_os (GogGame *self, GogOs os)
{
  return self->supports_os[os];
}

void
gog_game_set_supports_os (GogGame *self, GogOs os, gboolean supported)
{
  self->supports_os[os] = supported;
}

const char *const *
gog_game_get_languages (GogGame *self)
{
  return (const char *const *) self->languages;
}

void
gog_game_set_languages (GogGame *self, GStrv languages)
{
  g_strfreev (self->languages);
  self->languages = languages;
}

const char *const *
gog_game_get_categories (GogGame *self)
{
  return (const char *const *) self->categories;
}

void
gog_game_set_categories (GogGame *self, GStrv categories)
{
  gboolean same = (!self->categories && !categories) ||
                  (self->categories && categories &&
                   g_strv_equal ((const char *const *) self->categories,
                                 (const char *const *) categories));
  g_strfreev (self->categories);
  self->categories = categories;
  if (!same)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CATEGORIES]);
}

GogProductInfo *
gog_product_info_new (void)
{
  GogProductInfo *info = g_new0 (GogProductInfo, 1);
  info->screenshots = g_new0 (gchar *, 1);
  info->video_ids = g_new0 (gchar *, 1);
  info->messages = g_new0 (gchar *, 1);
  return info;
}

void
gog_product_info_free (GogProductInfo *info)
{
  if (!info)
    return;
  g_free (info->lead);
  g_free (info->changelog);
  g_free (info->store_url);
  g_free (info->support_url);
  g_free (info->forum_url);
  g_strfreev (info->screenshots);
  g_strfreev (info->video_ids);
  g_strfreev (info->messages);
  g_free (info);
}

const GogProductInfo *
gog_game_get_product_info (GogGame *self)
{
  return self->product;
}

void
gog_game_set_product_info (GogGame *self, GogProductInfo *info)
{
  g_return_if_fail (info != NULL);
  gog_product_info_free (self->product);
  self->product = info;
}

const GogStoreInfo *
gog_game_get_store_info (GogGame *self)
{
  return self->store;
}

void
gog_game_set_store_info (GogGame *self, GogStoreInfo *info)
{
  GogStoreInfo *old = self->store;
  gboolean same = gog_store_info_equal (old, info);
  self->store = info;
  gog_store_info_free (old);
  if (!same)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STORE]);
}

const GogGamesdbInfo *
gog_game_get_gamesdb (GogGame *self)
{
  return self->gamesdb;
}

void
gog_game_set_gamesdb (GogGame *self, GogGamesdbInfo *info)
{
  GogGamesdbInfo *old = self->gamesdb;
  gboolean same = gog_gamesdb_info_equal (old, info);
  self->gamesdb = info;
  gog_gamesdb_info_free (old);
  if (!same)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_GAMESDB]);
}

gint64
gog_game_get_release_date (GogGame *self)
{
  return self->release_date;
}

void
gog_game_set_release_date (GogGame *self, gint64 unix_time)
{
  self->release_date = unix_time;
}

GogProductType
gog_game_get_product_type (GogGame *self)
{
  return self->product_type;
}

void
gog_game_set_product_type (GogGame *self, GogProductType type)
{
  self->product_type = type;
}

const gint64 *
gog_game_get_related_ids (GogGame *self, guint *n)
{
  if (n)
    *n = self->related_ids->len;
  return (const gint64 *) self->related_ids->data;
}

void
gog_game_set_related_ids (GogGame *self, const gint64 *ids, guint n)
{
  g_array_set_size (self->related_ids, 0);
  if (ids && n)
    g_array_append_vals (self->related_ids, ids, n);
}

GPtrArray *
gog_game_get_installers (GogGame *self)
{
  return self->installers;
}

void
gog_game_add_installer (GogGame *self, GogInstallerInfo *info)
{
  g_ptr_array_add (self->installers, info);
}

static gint
compare_installer_language (gconstpointer a, gconstpointer b)
{
  const GogInstallerInfo *ia = *(const GogInstallerInfo *const *) a;
  const GogInstallerInfo *ib = *(const GogInstallerInfo *const *) b;
  return g_utf8_collate (ia->language_full ? ia->language_full : "",
                         ib->language_full ? ib->language_full : "");
}

static GStrv
dup_installer_languages (GogGame *self, gboolean codes)
{
  GPtrArray *sorted = g_ptr_array_new ();
  for (guint i = 0; i < self->installers->len; i++)
    {
      GogInstallerInfo *info = g_ptr_array_index (self->installers, i);
      if (info->language_full)
        g_ptr_array_add (sorted, info);
    }
  g_ptr_array_sort (sorted, compare_installer_language);

  GStrvBuilder *builder = g_strv_builder_new ();
  const char *last = NULL;
  for (guint i = 0; i < sorted->len; i++)
    {
      GogInstallerInfo *info = g_ptr_array_index (sorted, i);
      if (g_strcmp0 (info->language_full, last) == 0)
        continue;
      last = info->language_full;
      g_strv_builder_add (builder, codes ? (info->language_code ? info->language_code : "")
                                         : info->language_full);
    }
  g_ptr_array_unref (sorted);
  GStrv out = g_strv_builder_end (builder);
  g_strv_builder_unref (builder);
  return out;
}

GStrv
gog_game_dup_installer_languages (GogGame *self)
{
  return dup_installer_languages (self, FALSE);
}

GStrv
gog_game_dup_installer_language_codes (GogGame *self)
{
  return dup_installer_languages (self, TRUE);
}

GPtrArray *
gog_game_get_extras (GogGame *self)
{
  return self->extras;
}

void
gog_game_add_extra (GogGame *self, GogExtraInfo *info)
{
  g_ptr_array_add (self->extras, info);
}

guint64
gog_game_get_total_size (GogGame *self)
{
  guint64 total = 0;
  for (guint i = 0; i < self->installers->len; i++)
    {
      GogInstallerInfo *info = g_ptr_array_index (self->installers, i);
      total += info->size;
    }
  return total;
}

gboolean
gog_game_get_installed (GogGame *self)
{
  return self->installed;
}

void
gog_game_set_installed (GogGame *self, gboolean installed)
{
  if (self->installed == installed)
    return;
  self->installed = installed;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_INSTALLED]);
}

gint64
gog_game_get_last_played (GogGame *self)
{
  return self->last_played;
}

gint64
gog_game_get_play_seconds (GogGame *self)
{
  return self->play_seconds;
}

void
gog_game_set_play_history (GogGame *self, gint64 last_played, gint64 play_seconds)
{
  if (self->last_played == last_played && self->play_seconds == play_seconds)
    return;
  self->last_played = last_played;
  self->play_seconds = play_seconds;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PLAYED]);
}

const char *
gog_game_get_install_path (GogGame *self)
{
  return self->install_path;
}

void
gog_game_set_install_path (GogGame *self, const char *path)
{
  g_free (self->install_path);
  self->install_path = g_strdup (path);
}

GogRunnerKind
gog_game_get_runner_kind (GogGame *self)
{
  return self->runner_kind;
}

void
gog_game_set_runner_kind (GogGame *self, GogRunnerKind kind)
{
  self->runner_kind = kind;
}

const char *
gog_game_get_runner_path (GogGame *self)
{
  return self->runner_path;
}

void
gog_game_set_runner_path (GogGame *self, const char *path)
{
  g_free (self->runner_path);
  self->runner_path = g_strdup (path);
}

const char *
gog_game_get_prefix_path (GogGame *self)
{
  return self->prefix_path;
}

void
gog_game_set_prefix_path (GogGame *self, const char *path)
{
  g_free (self->prefix_path);
  self->prefix_path = g_strdup (path);
}

GogGame *
gog_game_get_base_game (GogGame *self)
{
  return self->base_game;
}

void
gog_game_set_base_game (GogGame *self, GogGame *base)
{
  g_set_object (&self->base_game, base);
}

const char *
gog_os_to_string (GogOs os)
{
  switch (os)
    {
    case GOG_OS_WINDOWS: return "windows";
    case GOG_OS_LINUX: return "linux";
    case GOG_OS_MAC: return "mac";
    default: return "unknown";
    }
}

const char *
gog_runner_kind_to_string (GogRunnerKind kind)
{
  switch (kind)
    {
    case GOG_RUNNER_NATIVE: return "native";
    case GOG_RUNNER_WINE: return "wine";
    case GOG_RUNNER_PROTON: return "proton";
    default: return "none";
    }
}

GogRunnerKind
gog_runner_kind_from_string (const char *str)
{
  if (g_strcmp0 (str, "native") == 0)
    return GOG_RUNNER_NATIVE;
  if (g_strcmp0 (str, "wine") == 0)
    return GOG_RUNNER_WINE;
  if (g_strcmp0 (str, "proton") == 0)
    return GOG_RUNNER_PROTON;
  return GOG_RUNNER_NONE;
}
