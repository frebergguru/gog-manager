/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#ifndef GOG_DIALOG_WINDOW_H
#define GOG_DIALOG_WINDOW_H

#include <adwaita.h>

G_BEGIN_DECLS

/* The app's dialogs are real windows rather than AdwDialogs, so they can be
 * moved and resized: modal over parent, Esc closes, at least min_width x
 * min_height. Put the content in with adw_window_set_content(). */
GtkWindow *gog_dialog_window_new (GtkWindow *parent, const char *title, int width, int height,
                                  int min_width, int min_height);

/* While FALSE, closing (Esc, the title bar's close button, gtk_window_close)
 * is refused -- e.g. mid-install, where Cancel is the way out. */
void gog_dialog_window_set_can_close (GtkWindow *window, gboolean can_close);

/* A confirmation in its own movable window, the counterpart of
 * AdwAlertDialog. Add responses in order (the last one sits rightmost),
 * then present it; callback gets the chosen response id once, after which
 * the window is gone. Closing it (Esc, the title bar) counts as the close
 * response. When it goes with its parent window instead, callback is not
 * called: the parent's data it would use may already be freed. */
typedef struct _GogAlert GogAlert;
typedef void (*GogAlertFunc) (const char *response, gpointer user_data);

GogAlert *gog_alert_new (const char *heading, const char *body);
void      gog_alert_add_response (GogAlert *alert, const char *id, const char *label,
                                  AdwResponseAppearance appearance);
void      gog_alert_set_default_response (GogAlert *alert, const char *id);
void      gog_alert_set_close_response (GogAlert *alert, const char *id);
/* Shown between the body and the buttons; the window takes it over. */
void      gog_alert_set_extra_child (GogAlert *alert, GtkWidget *child);
void      gog_alert_present (GogAlert *alert, GtkWindow *parent, GogAlertFunc callback,
                             gpointer user_data);

/* Tells screen readers message (gtk_accessible_announce, medium priority). Only through
 * AT-SPI: GTK's stand-in accessibility context, used when no AT-SPI bus is
 * there (Broadway, some headless sessions), crashes on announcements, and
 * nobody would hear them there anyway. */
void gog_announce (GtkWidget *widget, const char *message);

G_END_DECLS

#endif
