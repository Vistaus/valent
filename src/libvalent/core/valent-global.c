// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 Andy Holmes <andrew.g.r.holmes@gmail.com>

#define G_LOG_DOMAIN "valent-global"

#include "config.h"

#include <gio/gio.h>
#include <libpeas/peas.h>

#include "valent-global.h"
#include "valent-version.h"


static PeasEngine *default_engine = NULL;
static gboolean in_flatpak = FALSE;
static GThread *main_thread;


/*
 * libvalent Constructor
 */
#ifndef __has_attribute
# define __has_attribute(x) 0  /* Compatibility with non-clang compilers. */
#endif
#if __has_attribute(constructor)
static void __attribute__((constructor))
valent_init_ctor (void)
{
  in_flatpak = g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS);
  main_thread = g_thread_self ();
}
#else
# error Your platform/compiler is missing constructor support
#endif


/**
 * valent_get_main_thread: (skip)
 *
 * Get the main thread.
 *
 * Use the macro `VALENT_IS_MAIN_THREAD()` to determine whether or not the code
 * is currently running on the main thread.
 *
 * Returns: (transfer none): a #GThread
 *
 * Since: 1.0
 */
GThread *
valent_get_main_thread (void)
{
  return main_thread;
}

/**
 * valent_get_plugin_engine:
 *
 * Get the global #PeasEngine.
 *
 * The first call to this function initializes the #PeasEngine and adds
 * `resource:///plugins` to the search path, where emebedded plugins should be
 * registered. If running in a Flatpak, `/app/extensions/lib/valent/plugins`
 * will also be added to the search path for Flatpak extensions.
 *
 * Returns: (transfer none): a #PeasEngine
 *
 * Since: 1.0
 */
PeasEngine *
valent_get_plugin_engine (void)
{
  if (default_engine == NULL)
    {
      g_autoptr (GError) error = NULL;
      const GList *plugins = NULL;

      default_engine = peas_engine_get_default ();
      g_object_add_weak_pointer (G_OBJECT (default_engine),
                                 (gpointer)&default_engine);

      /* Ensure we have the minimum required typelibs for Python plugins */
      if (g_irepository_require (NULL, "Gio",  "2.0", 0, &error) &&
          g_irepository_require (NULL, "GLib", "2.0", 0, &error) &&
          g_irepository_require (NULL, "Json", "1.0", 0, &error) &&
          g_irepository_require (NULL, "Peas", "1.0", 0, &error) &&
          g_irepository_require (NULL, "Gdk",  "4.0", 0, &error) &&
          g_irepository_require (NULL, "Gtk",  "4.0", 0, &error) &&
          g_irepository_require (NULL, "Valent", VALENT_API_VERSION, 0, &error))
        peas_engine_enable_loader (default_engine, "python3");
      else
        g_message ("Disabling Python3 plugins: %s", error->message);

      /* Built-in Plugins */
      peas_engine_add_search_path (default_engine, "resource:///plugins", NULL);

      /* Flatpak Extensions */
      if (valent_in_flatpak ())
        {
          g_autofree char *flatpak_dir = NULL;

          flatpak_dir = g_build_filename ("/app", "extensions", "lib",
                                          "valent", "plugins", NULL);
          peas_engine_prepend_search_path (default_engine, flatpak_dir, NULL);
        }

      /* Load built-in plugins and Flatpak extensions automatically */
      plugins = peas_engine_get_plugin_list (default_engine);

      for (const GList *iter = plugins; iter; iter = iter->next)
        peas_engine_load_plugin (default_engine, iter->data);
    }

  return default_engine;
}

/**
 * valent_in_flatpak:
 *
 * Checks whether Valent is running in a Flatpak.
 *
 * Returns: %TRUE if running in a Flatpak, or %FALSE if not
 *
 * Since: 1.0
 */
gboolean
valent_in_flatpak (void)
{
  return in_flatpak;
}

