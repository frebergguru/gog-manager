#ifndef GOG_MAIN_WINDOW_H
#define GOG_MAIN_WINDOW_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GOG_TYPE_MAIN_WINDOW (gog_main_window_get_type ())
G_DECLARE_FINAL_TYPE (GogMainWindow, gog_main_window, GOG, MAIN_WINDOW, AdwApplicationWindow)

GogMainWindow *gog_main_window_new (GtkApplication *app);

G_END_DECLS

#endif
