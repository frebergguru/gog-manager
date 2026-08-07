/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Hypnotize
 */

#include <adwaita.h>
#include "ui/main_window.h"

extern GResource *gog_resources_get_resource (void);

static void
load_css (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_resource (provider, "/org/gogmanager/app/style.css");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                               GTK_STYLE_PROVIDER (provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

static void
on_activate (GApplication *app, gpointer user_data)
{
  (void) user_data;
  GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!window)
    {
      load_css ();
      window = GTK_WINDOW (gog_main_window_new (GTK_APPLICATION (app)));
    }
  gtk_window_present (window);
}

int
main (int argc, char *argv[])
{
  g_resources_register (gog_resources_get_resource ());

  AdwApplication *app =
      adw_application_new ("org.gogmanager.app", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
