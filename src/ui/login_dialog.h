/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_LOGIN_DIALOG_H
#define GOG_LOGIN_DIALOG_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Called once the window is closed; logged_in is TRUE if lgogdownloader is
 * logged in to GOG by then. */
typedef void (*GogLoginDoneFunc) (gboolean logged_in, gpointer user_data);

/* Logs lgogdownloader in to GOG without a terminal: it runs
 * `lgogdownloader --browser-login`, opens the GOG login page it gives in
 * the web browser, and passes on the address the browser ends up on, which
 * the user pastes. GOG Manager never sees the password. */
void gog_show_login_dialog (GtkWindow *parent, GogLoginDoneFunc on_done, gpointer user_data);

G_END_DECLS

#endif
