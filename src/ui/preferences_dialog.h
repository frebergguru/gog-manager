#ifndef GOG_PREFERENCES_DIALOG_H
#define GOG_PREFERENCES_DIALOG_H

#include <adwaita.h>
#include "../config.h"

G_BEGIN_DECLS

/* Called after Preferences is closed if the library path changed, so the
 * caller can trigger a rescan. */
typedef void (*GogLibraryPathChangedFunc) (const char *new_path, gpointer user_data);

void gog_show_preferences_dialog (GtkWindow *parent, GogConfig *config,
                                   GogLibraryPathChangedFunc on_library_path_changed,
                                   gpointer user_data);

G_END_DECLS

#endif
