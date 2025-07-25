/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

/* For the #GDesktopAppInfoLookup macros; since macro deprecation is implemented
 * in the preprocessor, we need to define this before including glib.h*/
#ifndef GLIB_DISABLE_DEPRECATION_WARNINGS
#define GLIB_DISABLE_DEPRECATION_WARNINGS
#endif

#include <string.h>

#include "giomodule.h"
#include "giomodule-priv.h"
#include "glib-private.h"
#include "glocalfilemonitor.h"
#include "gnativevolumemonitor.h"
#include "gproxyresolver.h"
#include "gproxy.h"
#include "gsettingsbackendinternal.h"
#include "ghttpproxy.h"
#include "gsocks4proxy.h"
#include "gsocks4aproxy.h"
#include "gsocks5proxy.h"
#include "gtlsbackend.h"
#include "gvfs.h"
#include "gnotificationbackend.h"
#include "ginitable.h"
#include "gnetworkmonitor.h"
#include "gdebugcontroller.h"
#include "gdebugcontrollerdbus.h"
#include "gmemorymonitor.h"
#include "gmemorymonitorportal.h"
#include "gmemorymonitordbus.h"
#ifdef __linux__
#include "gmemorymonitorpsi.h"
#endif
#ifdef HAVE_SYSINFO
#include "gmemorymonitorpoll.h"
#endif
#include "gpowerprofilemonitor.h"
#include "gpowerprofilemonitordbus.h"
#include "gpowerprofilemonitorportal.h"
#ifdef G_OS_WIN32
#include "gregistrysettingsbackend.h"
#include "giowin32-priv.h"
#endif
#include <glib/gstdio.h>

#if defined(G_OS_UNIX) && !defined(__APPLE__)
#include "gdesktopappinfo.h"
#endif
#ifdef HAVE_COCOA
#include "gosxappinfo.h"
#endif

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

#define __GLIB_H_INSIDE__
#include "gconstructor.h"
#undef __GLIB_H_INSIDE__

/**
 * GIOModule:
 *
 * Provides an interface and default functions for loading and unloading 
 * modules. This is used internally to make GIO extensible, but can also
 * be used by others to implement module loading.
 */

/**
 * GIOExtensionPoint:
 *
 * `GIOExtensionPoint` provides a mechanism for modules to extend the
 * functionality of the library or application that loaded it in an 
 * organized fashion.
 *
 * An extension point is identified by a name, and it may optionally
 * require that any implementation must be of a certain type (or derived
 * thereof). Use [func@Gio.IOExtensionPoint.register] to register an
 * extension point, and [method@Gio.IOExtensionPoint.set_required_type] to
 * set a required type.
 *
 * A module can implement an extension point by specifying the
 * [type@GObject.Type] that implements the functionality. Additionally, each
 * implementation of an extension point has a name, and a priority. Use
 * [func@Gio.IOExtensionPoint.implement] to implement an extension point.
 * 
 * ```c
 * GIOExtensionPoint *ep;
 *
 * // Register an extension point
 * ep = g_io_extension_point_register ("my-extension-point");
 * g_io_extension_point_set_required_type (ep, MY_TYPE_EXAMPLE);
 * ```
 *
 * ```c
 * // Implement an extension point
 * G_DEFINE_TYPE (MyExampleImpl, my_example_impl, MY_TYPE_EXAMPLE)
 * g_io_extension_point_implement ("my-extension-point",
 *                                 my_example_impl_get_type (),
 *                                 "my-example",
 *                                 10);
 * ```
 *
 *  It is up to the code that registered the extension point how
 *  it uses the implementations that have been associated with it.
 *  Depending on the use case, it may use all implementations, or
 *  only the one with the highest priority, or pick a specific
 *  one by name.
 *
 *  To avoid opening all modules just to find out what extension
 *  points they implement, GIO makes use of a caching mechanism,
 *  see [gio-querymodules](gio-querymodules.html).
 *  You are expected to run this command after installing a
 *  GIO module.
 *
 *  The `GIO_EXTRA_MODULES` environment variable can be used to
 *  specify additional directories to automatically load modules
 *  from. This environment variable has the same syntax as the
 *  `PATH`. If two modules have the same base name in different
 *  directories, then the latter one will be ignored. If additional
 *  directories are specified GIO will load modules from the built-in
 *  directory last.
 */

/**
 * GIOModuleScope:
 *
 * Represents a scope for loading IO modules. A scope can be used for blocking
 * duplicate modules, or blocking a module you don't want to load.
 *
 * The scope can be used with g_io_modules_load_all_in_directory_with_scope()
 * or g_io_modules_scan_all_in_directory_with_scope().
 *
 * Since: 2.30
 */
struct _GIOModuleScope {
  GIOModuleScopeFlags flags;
  GHashTable *basenames;
};

/**
 * g_io_module_scope_new:
 * @flags: flags for the new scope
 *
 * Create a new scope for loading of IO modules. A scope can be used for
 * blocking duplicate modules, or blocking a module you don't want to load.
 *
 * Specify the %G_IO_MODULE_SCOPE_BLOCK_DUPLICATES flag to block modules
 * which have the same base name as a module that has already been seen
 * in this scope.
 *
 * Returns: (transfer full): the new module scope
 *
 * Since: 2.30
 */
GIOModuleScope *
g_io_module_scope_new (GIOModuleScopeFlags flags)
{
  GIOModuleScope *scope = g_new0 (GIOModuleScope, 1);
  scope->flags = flags;
  scope->basenames = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  return scope;
}

/**
 * g_io_module_scope_free:
 * @scope: a module loading scope
 *
 * Free a module scope.
 *
 * Since: 2.30
 */
void
g_io_module_scope_free (GIOModuleScope *scope)
{
  if (!scope)
    return;
  g_hash_table_destroy (scope->basenames);
  g_free (scope);
}

/**
 * g_io_module_scope_block:
 * @scope: a module loading scope
 * @basename: the basename to block
 *
 * Block modules with the given @basename from being loaded when
 * this scope is used with g_io_modules_scan_all_in_directory_with_scope()
 * or g_io_modules_load_all_in_directory_with_scope().
 *
 * Since: 2.30
 */
void
g_io_module_scope_block (GIOModuleScope *scope,
                         const gchar    *basename)
{
  gchar *key;

  g_return_if_fail (scope != NULL);
  g_return_if_fail (basename != NULL);

  key = g_strdup (basename);
  g_hash_table_add (scope->basenames, key);
}

static gboolean
_g_io_module_scope_contains (GIOModuleScope *scope,
                             const gchar    *basename)
{
  return g_hash_table_contains (scope->basenames, basename);
}

struct _GIOModule {
  GTypeModule parent_instance;

  gchar       *filename;
  GModule     *library;
  gboolean     initialized; /* The module was loaded at least once */

  void (* load)   (GIOModule *module);
  void (* unload) (GIOModule *module);
};

struct _GIOModuleClass
{
  GTypeModuleClass parent_class;

};

static void      g_io_module_finalize      (GObject      *object);
static gboolean  g_io_module_load_module   (GTypeModule  *gmodule);
static void      g_io_module_unload_module (GTypeModule  *gmodule);

/**
 * GIOExtension:
 *
 * #GIOExtension is an opaque data structure and can only be accessed
 * using the following functions.
 */
struct _GIOExtension {
  char *name;
  GType type;
  gint priority;
};

struct _GIOExtensionPoint {
  GType required_type;
  char *name;
  GList *extensions;
  GList *lazy_load_modules;
};

static GHashTable *extension_points = NULL;
G_LOCK_DEFINE_STATIC(extension_points);

G_DEFINE_TYPE (GIOModule, g_io_module, G_TYPE_TYPE_MODULE)

static void
g_io_module_class_init (GIOModuleClass *class)
{
  GObjectClass     *object_class      = G_OBJECT_CLASS (class);
  GTypeModuleClass *type_module_class = G_TYPE_MODULE_CLASS (class);

  object_class->finalize     = g_io_module_finalize;

  type_module_class->load    = g_io_module_load_module;
  type_module_class->unload  = g_io_module_unload_module;
}

static void
g_io_module_init (GIOModule *module)
{
}

static void
g_io_module_finalize (GObject *object)
{
  GIOModule *module = G_IO_MODULE (object);

  g_free (module->filename);

  G_OBJECT_CLASS (g_io_module_parent_class)->finalize (object);
}

static gboolean
load_symbols (GIOModule *module)
{
  gchar *name;
  gchar *load_symname;
  gchar *unload_symname;
  gboolean ret;

  name = _g_io_module_extract_name (module->filename);
  load_symname = g_strconcat ("g_io_", name, "_load", NULL);
  unload_symname = g_strconcat ("g_io_", name, "_unload", NULL);

  ret = g_module_symbol (module->library,
                         load_symname,
                         (gpointer) &module->load) &&
        g_module_symbol (module->library,
                         unload_symname,
                         (gpointer) &module->unload);

  if (!ret)
    {
      /* Fallback to old names */
      ret = g_module_symbol (module->library,
                             "g_io_module_load",
                             (gpointer) &module->load) &&
            g_module_symbol (module->library,
                             "g_io_module_unload",
                             (gpointer) &module->unload);
    }

  g_free (name);
  g_free (load_symname);
  g_free (unload_symname);

  return ret;
}

static gboolean
g_io_module_load_module (GTypeModule *gmodule)
{
  GIOModule *module = G_IO_MODULE (gmodule);
  GError *error = NULL;

  if (!module->filename)
    {
      g_warning ("GIOModule path not set");
      return FALSE;
    }

  module->library = g_module_open_full (module->filename, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL, &error);

  if (!module->library)
    {
      g_printerr ("%s\n", error->message);
      g_clear_error (&error);
      return FALSE;
    }

  /* Make sure that the loaded library contains the required methods */
  if (!load_symbols (module))
    {
      g_printerr ("%s\n", g_module_error ());
      g_module_close (module->library);

      return FALSE;
    }

  /* Initialize the loaded module */
  module->load (module);
  module->initialized = TRUE;

  return TRUE;
}

static void
g_io_module_unload_module (GTypeModule *gmodule)
{
  GIOModule *module = G_IO_MODULE (gmodule);

  module->unload (module);

  g_module_close (module->library);
  module->library = NULL;

  module->load   = NULL;
  module->unload = NULL;
}

/**
 * g_io_module_new:
 * @filename: (type filename): filename of the shared library module.
 * 
 * Creates a new GIOModule that will load the specific
 * shared library when in use.
 * 
 * Returns: a #GIOModule from given @filename, 
 * or %NULL on error.
 **/
GIOModule *
g_io_module_new (const gchar *filename)
{
  GIOModule *module;

  g_return_val_if_fail (filename != NULL, NULL);

  module = g_object_new (G_IO_TYPE_MODULE, NULL);
  module->filename = g_strdup (filename);

  return module;
}

static gboolean
is_valid_module_name (const gchar        *basename,
                      GIOModuleScope     *scope)
{
  gboolean result;

#if !defined(G_OS_WIN32) && !defined(G_WITH_CYGWIN)
  #if defined(__APPLE__)
  if (!g_str_has_prefix (basename, "lib") ||
      !(g_str_has_suffix (basename, ".so") ||
        g_str_has_suffix (basename, ".dylib")))
    return FALSE;
  #else
  if (!g_str_has_prefix (basename, "lib") ||
      !g_str_has_suffix (basename, ".so"))
    return FALSE;
  #endif
#else
  if (!g_str_has_suffix (basename, ".dll"))
    return FALSE;
#endif

  result = TRUE;
  if (scope)
    {
      result = _g_io_module_scope_contains (scope, basename) ? FALSE : TRUE;
      if (result && (scope->flags & G_IO_MODULE_SCOPE_BLOCK_DUPLICATES))
        g_io_module_scope_block (scope, basename);
    }

  return result;
}


/**
 * g_io_modules_scan_all_in_directory_with_scope:
 * @dirname: (type filename): pathname for a directory containing modules
 *     to scan.
 * @scope: a scope to use when scanning the modules
 *
 * Scans all the modules in the specified directory, ensuring that
 * any extension point implemented by a module is registered.
 *
 * This may not actually load and initialize all the types in each
 * module, some modules may be lazily loaded and initialized when
 * an extension point it implements is used with e.g.
 * g_io_extension_point_get_extensions() or
 * g_io_extension_point_get_extension_by_name().
 *
 * If you need to guarantee that all types are loaded in all the modules,
 * use g_io_modules_load_all_in_directory().
 *
 * Since: 2.30
 **/
void
g_io_modules_scan_all_in_directory_with_scope (const char     *dirname,
                                               GIOModuleScope *scope)
{
  const gchar *name;
  char *filename;
  GDir *dir;
  GStatBuf statbuf;
  char *data;
  time_t cache_time;
  GHashTable *cache;

  if (!g_module_supported ())
    return;

  dir = g_dir_open (dirname, 0, NULL);
  if (!dir)
    return;

  filename = g_build_filename (dirname, "giomodule.cache", NULL);

  cache = NULL;
  cache_time = 0;
  if (g_stat (filename, &statbuf) == 0 &&
      g_file_get_contents (filename, &data, NULL, NULL))
    {
      char **lines;
      int i;

      /* cache_time is the time the cache file was created; we also take
       * into account the change time because in ostree based systems, all
       * system file have mtime equal to epoch 0.
       *
       * Any file that has a ctime before this was created then and not modified
       * since then (userspace can't change ctime). Its possible to change the
       * ctime forward without changing the file content, by e.g.  chmoding the
       * file, but this is uncommon and will only cause us to not use the cache
       * so will not cause bugs.
       */
      cache_time = MAX(statbuf.st_mtime, statbuf.st_ctime);

      lines = g_strsplit (data, "\n", -1);
      g_free (data);

      for (i = 0;  lines[i] != NULL; i++)
	{
	  char *line = lines[i];
	  char *file;
	  char *colon;
	  char **strv_extension_points;

	  if (line[0] == '#')
	    continue;

	  colon = strchr (line, ':');
	  if (colon == NULL || line == colon)
	    continue; /* Invalid line, ignore */

	  *colon = 0; /* terminate filename */
	  file = g_strdup (line);
	  colon++; /* after colon */

	  while (g_ascii_isspace (*colon))
	    colon++;

          if (G_UNLIKELY (!cache))
            cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, (GDestroyNotify)g_strfreev);

	  strv_extension_points = g_strsplit (colon, ",", -1);
	  g_hash_table_insert (cache, file, strv_extension_points);
	}
      g_strfreev (lines);
    }

  while ((name = g_dir_read_name (dir)))
    {
      if (is_valid_module_name (name, scope))
	{
	  GIOExtensionPoint *extension_point;
	  GIOModule *module;
	  gchar *path;
	  char **strv_extension_points = NULL;
	  int i;

	  path = g_build_filename (dirname, name, NULL);
	  module = g_io_module_new (path);

          if (cache)
            strv_extension_points = g_hash_table_lookup (cache, name);

	  if (strv_extension_points != NULL &&
	      g_stat (path, &statbuf) == 0 &&
	      statbuf.st_ctime <= cache_time)
	    {
	      /* Lazy load/init the library when first required */
	      for (i = 0; strv_extension_points[i] != NULL; i++)
		{
		  extension_point =
		    g_io_extension_point_register (strv_extension_points[i]);
		  extension_point->lazy_load_modules =
		    g_list_prepend (extension_point->lazy_load_modules,
				    module);
		}
	    }
	  else
	    {
	      /* Try to load and init types */
	      if (g_type_module_use (G_TYPE_MODULE (module)))
		{
		  g_type_module_unuse (G_TYPE_MODULE (module)); /* Unload */
		  /* module must remain alive, because the type system keeps weak refs */
		  g_ignore_leak (module);
		}
	      else
		{
		  g_printerr ("Failed to load module: %s\n", path);
		  g_object_unref (module);
		}
	    }

	  g_free (path);
	}
    }

  g_dir_close (dir);

  if (cache)
    g_hash_table_destroy (cache);

  g_free (filename);
}

/**
 * g_io_modules_scan_all_in_directory:
 * @dirname: (type filename): pathname for a directory containing modules
 *     to scan.
 *
 * Scans all the modules in the specified directory, ensuring that
 * any extension point implemented by a module is registered.
 *
 * This may not actually load and initialize all the types in each
 * module, some modules may be lazily loaded and initialized when
 * an extension point it implements is used with e.g.
 * g_io_extension_point_get_extensions() or
 * g_io_extension_point_get_extension_by_name().
 *
 * If you need to guarantee that all types are loaded in all the modules,
 * use g_io_modules_load_all_in_directory().
 *
 * Since: 2.24
 **/
void
g_io_modules_scan_all_in_directory (const char *dirname)
{
  g_io_modules_scan_all_in_directory_with_scope (dirname, NULL);
}

/**
 * g_io_modules_load_all_in_directory_with_scope:
 * @dirname: (type filename): pathname for a directory containing modules
 *     to load.
 * @scope: a scope to use when scanning the modules.
 *
 * Loads all the modules in the specified directory.
 *
 * If don't require all modules to be initialized (and thus registering
 * all gtypes) then you can use g_io_modules_scan_all_in_directory()
 * which allows delayed/lazy loading of modules.
 *
 * Returns: (element-type GIOModule) (transfer full): a list of #GIOModules loaded
 *      from the directory,
 *      All the modules are loaded into memory, if you want to
 *      unload them (enabling on-demand loading) you must call
 *      g_type_module_unuse() on all the modules. Free the list
 *      with g_list_free().
 *
 * Since: 2.30
 **/
GList *
g_io_modules_load_all_in_directory_with_scope (const char     *dirname,
                                               GIOModuleScope *scope)
{
  const gchar *name;
  GDir        *dir;
  GList *modules;

  if (!g_module_supported ())
    return NULL;

  dir = g_dir_open (dirname, 0, NULL);
  if (!dir)
    return NULL;

  modules = NULL;
  while ((name = g_dir_read_name (dir)))
    {
      if (is_valid_module_name (name, scope))
        {
          GIOModule *module;
          gchar     *path;

          path = g_build_filename (dirname, name, NULL);
          module = g_io_module_new (path);

          if (!g_type_module_use (G_TYPE_MODULE (module)))
            {
              g_printerr ("Failed to load module: %s\n", path);
              g_object_unref (module);
              g_free (path);
              continue;
            }
	  
          g_free (path);

          modules = g_list_prepend (modules, module);
        }
    }
  
  g_dir_close (dir);

  return modules;
}

/**
 * g_io_modules_load_all_in_directory:
 * @dirname: (type filename): pathname for a directory containing modules
 *     to load.
 *
 * Loads all the modules in the specified directory.
 *
 * If don't require all modules to be initialized (and thus registering
 * all gtypes) then you can use g_io_modules_scan_all_in_directory()
 * which allows delayed/lazy loading of modules.
 *
 * Returns: (element-type GIOModule) (transfer full): a list of #GIOModules loaded
 *      from the directory,
 *      All the modules are loaded into memory, if you want to
 *      unload them (enabling on-demand loading) you must call
 *      g_type_module_unuse() on all the modules. Free the list
 *      with g_list_free().
 **/
GList *
g_io_modules_load_all_in_directory (const char *dirname)
{
  return g_io_modules_load_all_in_directory_with_scope (dirname, NULL);
}

static gpointer
try_class (GIOExtension *extension,
           guint         is_supported_offset)
{
  GType type = g_io_extension_get_type (extension);
  typedef gboolean (*verify_func) (void);
  gpointer class;

  class = g_type_class_ref (type);
  if (!is_supported_offset || (* G_STRUCT_MEMBER(verify_func, class, is_supported_offset)) ())
    return class;

  g_type_class_unref (class);
  return NULL;
}

static void
print_help (const char        *envvar,
            GIOExtensionPoint *ep)
{
  g_print ("Supported arguments for %s environment variable:\n", envvar);

  if (g_io_extension_point_get_extensions (ep) == NULL)
    g_print (" (none)\n");
  else
    {
      GList *l;
      GIOExtension *extension;
      gsize width = 0;

      for (l = g_io_extension_point_get_extensions (ep); l; l = l->next)
        {
          extension = l->data;
          width = MAX (width, strlen (g_io_extension_get_name (extension)));
        }

      for (l = g_io_extension_point_get_extensions (ep); l; l = l->next)
        {
          extension = l->data;

          g_print (" %*s - %d\n", (int) MIN (width, G_MAXINT),
                   g_io_extension_get_name (extension),
                   g_io_extension_get_priority (extension));
        }
    }
}

/**
 * _g_io_module_get_default_type:
 * @extension_point: the name of an extension point
 * @envvar: (nullable): the name of an environment variable to
 *     override the default implementation.
 * @is_supported_offset: a vtable offset, or zero
 *
 * Retrieves the default class implementing @extension_point.
 *
 * If @envvar is not %NULL, and the environment variable with that
 * name is set, then the implementation it specifies will be tried
 * first. After that, or if @envvar is not set, all other
 * implementations will be tried in order of decreasing priority.
 *
 * If @is_supported_offset is non-zero, then it is the offset into the
 * class vtable at which there is a function that takes no arguments and
 * returns a boolean.  This function will be called on each candidate
 * implementation to check if it is actually usable or not.
 *
 * The result is cached after it is generated the first time, and
 * the function is thread-safe.
 *
 * Returns: (transfer none): the type to instantiate to implement
 *     @extension_point, or %G_TYPE_INVALID if there are no usable
 *     implementations.
 */
GType
_g_io_module_get_default_type (const gchar *extension_point,
                               const gchar *envvar,
                               guint        is_supported_offset)
{
  static GRecMutex default_modules_lock;
  static GHashTable *default_modules;
  const char *use_this;
  GList *l;
  GIOExtensionPoint *ep;
  GIOExtension *extension, *preferred;
  gpointer impl;

  g_rec_mutex_lock (&default_modules_lock);
  if (default_modules)
    {
      gpointer key;

      if (g_hash_table_lookup_extended (default_modules, extension_point, &key, &impl))
        {
          g_rec_mutex_unlock (&default_modules_lock);
          return impl ? G_OBJECT_CLASS_TYPE (impl) : G_TYPE_INVALID;
        }
    }
  else
    {
      default_modules = g_hash_table_new (g_str_hash, g_str_equal);
    }

  _g_io_modules_ensure_loaded ();
  ep = g_io_extension_point_lookup (extension_point);

  if (!ep)
    {
      g_warn_if_reached ();
      g_rec_mutex_unlock (&default_modules_lock);
      return G_TYPE_INVALID;
    }

  /* It’s OK to query the environment here, even when running as setuid, because
   * it only allows a choice between existing already-loaded modules. No new
   * code is loaded based on the environment variable value. */
  use_this = envvar ? g_getenv (envvar) : NULL;
  if (g_strcmp0 (use_this, "help") == 0)
    {
      print_help (envvar, ep);
      use_this = NULL;
    }

  if (use_this)
    {
      preferred = g_io_extension_point_get_extension_by_name (ep, use_this);
      if (preferred)
        {
          impl = try_class (preferred, is_supported_offset);
          if (impl)
            goto done;
        }
      else
        g_warning ("Can't find module '%s' specified in %s", use_this, envvar);
    }
  else
    preferred = NULL;

  for (l = g_io_extension_point_get_extensions (ep); l != NULL; l = l->next)
    {
      extension = l->data;
      if (extension == preferred)
        continue;

      impl = try_class (extension, is_supported_offset);
      if (impl)
        goto done;
    }

  impl = NULL;

 done:
  g_hash_table_insert (default_modules, g_strdup (extension_point), impl);
  g_rec_mutex_unlock (&default_modules_lock);

  return impl ? G_OBJECT_CLASS_TYPE (impl) : G_TYPE_INVALID;
}

static gpointer
try_implementation (const char           *extension_point,
                    GIOExtension         *extension,
		    GIOModuleVerifyFunc   verify_func)
{
  GType type = g_io_extension_get_type (extension);
  gpointer impl;

  if (g_type_is_a (type, G_TYPE_INITABLE))
    {
      GError *error = NULL;

      impl = g_initable_new (type, NULL, &error, NULL);
      if (impl)
        return impl;

      g_debug ("Failed to initialize %s (%s) for %s: %s",
               g_io_extension_get_name (extension),
               g_type_name (type),
               extension_point,
               error ? error->message : "");
      g_clear_error (&error);
      return NULL;
    }
  else
    {
      impl = g_object_new (type, NULL);
      if (!verify_func || verify_func (impl))
	return impl;

      g_object_unref (impl);
      return NULL;
    }
}

static void
weak_ref_free (GWeakRef *weak_ref)
{
  g_weak_ref_clear (weak_ref);
  g_free (weak_ref);
}

/**
 * _g_io_module_get_default:
 * @extension_point: the name of an extension point
 * @envvar: (nullable): the name of an environment variable to
 *     override the default implementation.
 * @verify_func: (nullable): a function to call to verify that
 *     a given implementation is usable in the current environment.
 *
 * Retrieves the default object implementing @extension_point.
 *
 * If @envvar is not %NULL, and the environment variable with that
 * name is set, then the implementation it specifies will be tried
 * first. After that, or if @envvar is not set, all other
 * implementations will be tried in order of decreasing priority.
 *
 * If an extension point implementation implements #GInitable, then
 * that implementation will only be used if it initializes
 * successfully. Otherwise, if @verify_func is not %NULL, then it will
 * be called on each candidate implementation after construction, to
 * check if it is actually usable or not.
 *
 * The result is cached after it is generated the first time (but the cache does
 * not keep a strong reference to the object), and
 * the function is thread-safe.
 *
 * Returns: (transfer full) (nullable): an object implementing
 *     @extension_point, or %NULL if there are no usable
 *     implementations.
 */
gpointer
_g_io_module_get_default (const gchar         *extension_point,
			  const gchar         *envvar,
			  GIOModuleVerifyFunc  verify_func)
{
  static GRecMutex default_modules_lock;
  static GHashTable *default_modules;
  const char *use_this;
  GList *l;
  GIOExtensionPoint *ep;
  GIOExtension *extension = NULL, *preferred;
  gpointer impl, value;
  GWeakRef *impl_weak_ref = NULL;

  g_rec_mutex_lock (&default_modules_lock);
  if (default_modules)
    {
      if (g_hash_table_lookup_extended (default_modules, extension_point,
                                        NULL, &value))
        {
          /* Don’t debug here, since we’re returning a cached object which was
           * already printed earlier. */
          impl_weak_ref = value;
          impl = g_weak_ref_get (impl_weak_ref);

          /* If the object has been finalised (impl == NULL), fall through and
           * instantiate a new one. */
          if (impl != NULL)
            {
              g_rec_mutex_unlock (&default_modules_lock);
              return g_steal_pointer (&impl);
            }
        }
    }
  else
    {
      default_modules = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, (GDestroyNotify) weak_ref_free);
    }

  _g_io_modules_ensure_loaded ();
  ep = g_io_extension_point_lookup (extension_point);

  if (!ep)
    {
      g_debug ("%s: Failed to find extension point ‘%s’",
               G_STRFUNC, extension_point);
      g_warn_if_reached ();
      g_rec_mutex_unlock (&default_modules_lock);
      return NULL;
    }

  /* It’s OK to query the environment here, even when running as setuid, because
   * it only allows a choice between existing already-loaded modules. No new
   * code is loaded based on the environment variable value. */
  use_this = envvar ? g_getenv (envvar) : NULL;
  if (g_strcmp0 (use_this, "help") == 0)
    {
      print_help (envvar, ep);
      use_this = NULL;
    }

  if (use_this)
    {
      preferred = g_io_extension_point_get_extension_by_name (ep, use_this);
      if (preferred)
	{
	  impl = try_implementation (extension_point, preferred, verify_func);
	  extension = preferred;
	  if (impl)
	    goto done;
	}
      else
        g_warning ("Can't find module '%s' specified in %s", use_this, envvar);
    }
  else
    preferred = NULL;

  for (l = g_io_extension_point_get_extensions (ep); l != NULL; l = l->next)
    {
      extension = l->data;
      if (extension == preferred)
	continue;

      impl = try_implementation (extension_point, extension, verify_func);
      if (impl)
	goto done;
    }

  impl = NULL;

 done:
  if (impl_weak_ref == NULL)
    {
      impl_weak_ref = g_new0 (GWeakRef, 1);
      g_weak_ref_init (impl_weak_ref, impl);
      g_hash_table_insert (default_modules, g_strdup (extension_point),
                           g_steal_pointer (&impl_weak_ref));
    }
  else
    {
      g_weak_ref_set (impl_weak_ref, impl);
    }

  g_rec_mutex_unlock (&default_modules_lock);

  if (impl != NULL)
    {
      g_assert (extension != NULL);
      g_debug ("%s: Found default implementation %s (%s) for ‘%s’",
               G_STRFUNC, g_io_extension_get_name (extension),
               G_OBJECT_TYPE_NAME (impl), extension_point);
    }
  else
    g_debug ("%s: Failed to find default implementation for ‘%s’",
             G_STRFUNC, extension_point);

  return g_steal_pointer (&impl);
}

extern GType g_inotify_file_monitor_get_type (void);
extern GType g_kqueue_file_monitor_get_type (void);
extern GType g_win32_file_monitor_get_type (void);

extern GType _g_unix_volume_monitor_get_type (void);
extern GType _g_local_vfs_get_type (void);

extern GType _g_win32_volume_monitor_get_type (void);
extern GType _g_winhttp_vfs_get_type (void);

extern GType _g_dummy_proxy_resolver_get_type (void);
extern GType _g_dummy_tls_backend_get_type (void);
extern GType g_network_monitor_base_get_type (void);
#ifdef HAVE_NETLINK
extern GType _g_network_monitor_netlink_get_type (void);
extern GType _g_network_monitor_nm_get_type (void);
#endif

extern GType g_debug_controller_dbus_get_type (void);
extern GType g_memory_monitor_dbus_get_type (void);
#ifdef __linux__
extern GType g_memory_monitor_psi_get_type (void);
#endif
#ifdef HAVE_SYSINFO
extern GType g_memory_monitor_poll_get_type (void);
#endif
extern GType g_memory_monitor_portal_get_type (void);
extern GType g_memory_monitor_win32_get_type (void);
extern GType g_power_profile_monitor_dbus_get_type (void);

#ifdef G_OS_UNIX
extern GType g_fdo_notification_backend_get_type (void);
extern GType g_gtk_notification_backend_get_type (void);
extern GType g_portal_notification_backend_get_type (void);
extern GType g_proxy_resolver_portal_get_type (void);
extern GType g_network_monitor_portal_get_type (void);
#endif

#ifdef HAVE_COCOA
extern GType g_cocoa_notification_backend_get_type (void);
extern GType g_osx_network_monitor_get_type (void);
#endif

#ifdef G_PLATFORM_WIN32
extern GType g_win32_notification_backend_get_type (void);

#include <windows.h>
extern GType _g_win32_network_monitor_get_type (void);

static HMODULE gio_dll = NULL;

#ifndef GLIB_STATIC_COMPILATION

BOOL WINAPI DllMain (HINSTANCE hinstDLL,
                     DWORD     fdwReason,
                     LPVOID    lpvReserved);

BOOL WINAPI
DllMain (HINSTANCE hinstDLL,
	 DWORD     fdwReason,
	 LPVOID    lpvReserved)
{
  if (fdwReason == DLL_PROCESS_ATTACH)
    {
      gio_dll = hinstDLL;
      gio_win32_appinfo_init (FALSE);
    }

  return TRUE;
}

#elif defined(G_HAS_CONSTRUCTORS) /* && G_PLATFORM_WIN32 && GLIB_STATIC_COMPILATION */
extern void glib_win32_init (void);
extern void gobject_win32_init (void);

#ifdef G_DEFINE_CONSTRUCTOR_NEEDS_PRAGMA
#pragma G_DEFINE_CONSTRUCTOR_PRAGMA_ARGS(giomodule_init_ctor)
#endif

G_DEFINE_CONSTRUCTOR (giomodule_init_ctor)

static void
giomodule_init_ctor (void)
{
  /* When built dynamically, module initialization is done through DllMain
   * function which is called when the dynamic library is loaded by the glib
   * module AFTER loading gobject. So, in dynamic configuration glib and
   * gobject are always initialized BEFORE gio.
   *
   * When built statically, initialization mechanism relies on hooking
   * functions to the CRT section directly at compilation time. As we don't
   * control how each compilation unit will be built and in which order, we
   * obtain the same kind of issue as the "static initialization order fiasco".
   * In this case, we must ensure explicitly that glib and gobject are always
   * well initialized BEFORE gio.
   */
  glib_win32_init ();
  gobject_win32_init ();
  gio_win32_appinfo_init (FALSE);
}

#else /* G_PLATFORM_WIN32 && GLIB_STATIC_COMPILATION && !G_HAS_CONSTRUCTORS */
#error Your platform/compiler is missing constructor support
#endif /* GLIB_STATIC_COMPILATION */

void *
_g_io_win32_get_module (void)
{
  if (!gio_dll)
    GetModuleHandleEx (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) _g_io_win32_get_module,
                       &gio_dll);
  return gio_dll;
}

#endif /* G_PLATFORM_WIN32 */

void
_g_io_modules_ensure_extension_points_registered (void)
{
  static gsize registered_extensions = FALSE;
  GIOExtensionPoint *ep;

  if (g_once_init_enter (&registered_extensions))
    {
#if defined(G_OS_UNIX) && !defined(__APPLE__)
#if !GLIB_CHECK_VERSION (3, 0, 0)
      ep = g_io_extension_point_register (G_DESKTOP_APP_INFO_LOOKUP_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_DESKTOP_APP_INFO_LOOKUP);
#endif
#endif

      ep = g_io_extension_point_register (G_LOCAL_FILE_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_LOCAL_FILE_MONITOR);

      ep = g_io_extension_point_register (G_NFS_FILE_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_LOCAL_FILE_MONITOR);

      ep = g_io_extension_point_register (G_VOLUME_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_VOLUME_MONITOR);
      
      ep = g_io_extension_point_register (G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_NATIVE_VOLUME_MONITOR);
      
      ep = g_io_extension_point_register (G_VFS_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_VFS);

      ep = g_io_extension_point_register ("gsettings-backend");
      g_io_extension_point_set_required_type (ep, G_TYPE_OBJECT);

      ep = g_io_extension_point_register (G_PROXY_RESOLVER_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_PROXY_RESOLVER);

      ep = g_io_extension_point_register (G_PROXY_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_PROXY);

      ep = g_io_extension_point_register (G_TLS_BACKEND_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_TLS_BACKEND);

      ep = g_io_extension_point_register (G_NETWORK_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_NETWORK_MONITOR);

      ep = g_io_extension_point_register (G_NOTIFICATION_BACKEND_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_NOTIFICATION_BACKEND);

      ep = g_io_extension_point_register (G_DEBUG_CONTROLLER_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_DEBUG_CONTROLLER);

      ep = g_io_extension_point_register (G_MEMORY_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_MEMORY_MONITOR);

      ep = g_io_extension_point_register (G_POWER_PROFILE_MONITOR_EXTENSION_POINT_NAME);
      g_io_extension_point_set_required_type (ep, G_TYPE_POWER_PROFILE_MONITOR);

      g_once_init_leave (&registered_extensions, TRUE);
    }
}

static gchar *
get_gio_module_dir (void)
{
  gchar *module_dir;
  gboolean is_setuid = GLIB_PRIVATE_CALL (g_check_setuid) ();

  /* If running as setuid, loading modules from an arbitrary directory
   * controlled by the unprivileged user who is running the program could allow
   * for execution of arbitrary code (in constructors in modules).
   * Don’t allow it.
   *
   * If a setuid program somehow needs to load additional GIO modules, it should
   * explicitly call g_io_modules_scan_all_in_directory(). */
  module_dir = !is_setuid ? g_strdup (g_getenv ("GIO_MODULE_DIR")) : NULL;
  if (module_dir == NULL)
    {
#ifdef G_OS_WIN32
      gchar *install_dir;

      install_dir = g_win32_get_package_installation_directory_of_module (gio_dll);
      module_dir = g_build_filename (install_dir,
                                     "lib", "gio", "modules",
                                     NULL);
      g_free (install_dir);
#else
      module_dir = g_strdup (GIO_MODULE_DIR);
#ifdef __APPLE__
#include "TargetConditionals.h"
/* Only auto-relocate on macOS, not watchOS etc; older macOS SDKs only define TARGET_OS_MAC */
#if (defined (TARGET_OS_OSX) && TARGET_OS_OSX) || \
     (!defined (TARGET_OS_OSX) && defined (TARGET_OS_MAC) && TARGET_OS_MAC)
#include <dlfcn.h>
      {
        g_autofree gchar *path = NULL;
        g_autofree gchar *possible_dir = NULL;
        Dl_info info;

        if (dladdr (get_gio_module_dir, &info))
          {
            /* Gets path to the PREFIX/lib directory */
            path = g_path_get_dirname (info.dli_fname);
            possible_dir = g_build_filename (path, "gio", "modules", NULL);
            if (g_file_test (possible_dir, G_FILE_TEST_IS_DIR))
              {
                g_free (module_dir);
                module_dir = g_steal_pointer (&possible_dir);
              }
          }
      }
#endif
#endif
#endif
    }

  return module_dir;
}

void
_g_io_modules_ensure_loaded (void)
{
  static gsize loaded_dirs = FALSE;
  const char *module_path;
  GIOModuleScope *scope;

  _g_io_modules_ensure_extension_points_registered ();

  if (g_once_init_enter (&loaded_dirs))
    {
      gboolean is_setuid = GLIB_PRIVATE_CALL (g_check_setuid) ();
      gchar *module_dir;

      scope = g_io_module_scope_new (G_IO_MODULE_SCOPE_BLOCK_DUPLICATES);

      /* First load any overrides, extras (but not if running as setuid!) */
      module_path = !is_setuid ? g_getenv ("GIO_EXTRA_MODULES") : NULL;
      if (module_path)
	{
	  gchar **paths;
	  int i;

	  paths = g_strsplit (module_path, G_SEARCHPATH_SEPARATOR_S, 0);

	  for (i = 0; paths[i] != NULL; i++)
	    {
	      g_io_modules_scan_all_in_directory_with_scope (paths[i], scope);
	    }

	  g_strfreev (paths);
	}

      /* Then load the compiled in path */
      module_dir = get_gio_module_dir ();

      g_io_modules_scan_all_in_directory_with_scope (module_dir, scope);
      g_free (module_dir);

      g_io_module_scope_free (scope);

      /* Initialize types from built-in "modules" */
      g_type_ensure (g_null_settings_backend_get_type ());
      g_type_ensure (g_memory_settings_backend_get_type ());
      g_type_ensure (g_keyfile_settings_backend_get_type ());
      g_type_ensure (g_power_profile_monitor_dbus_get_type ());
#if defined(FILE_MONITOR_BACKEND_INOTIFY) || defined(FILE_MONITOR_BACKEND_LIBINOTIFY_KQUEUE)
      g_type_ensure (g_inotify_file_monitor_get_type ());
#endif
#if defined(FILE_MONITOR_BACKEND_KQUEUE)
      g_type_ensure (g_kqueue_file_monitor_get_type ());
#endif
#ifdef G_OS_WIN32
      g_type_ensure (_g_win32_volume_monitor_get_type ());
      g_type_ensure (g_win32_file_monitor_get_type ());
      g_type_ensure (g_registry_settings_backend_get_type ());
#endif
#ifdef HAVE_COCOA
      g_type_ensure (g_cocoa_notification_backend_get_type ());
      g_type_ensure (g_nextstep_settings_backend_get_type ());
      g_type_ensure (g_osx_app_info_get_type ());
      g_type_ensure (g_osx_network_monitor_get_type ());
#endif
#ifdef G_OS_UNIX
      g_type_ensure (_g_unix_volume_monitor_get_type ());
      g_type_ensure (g_debug_controller_dbus_get_type ());
      g_type_ensure (g_fdo_notification_backend_get_type ());
      g_type_ensure (g_gtk_notification_backend_get_type ());
      g_type_ensure (g_portal_notification_backend_get_type ());
      g_type_ensure (g_memory_monitor_dbus_get_type ());
#ifdef __linux__
      g_type_ensure (g_memory_monitor_psi_get_type ());
#endif
#ifdef HAVE_SYSINFO
      g_type_ensure (g_memory_monitor_poll_get_type ());
#endif
      g_type_ensure (g_memory_monitor_portal_get_type ());
      g_type_ensure (g_network_monitor_portal_get_type ());
      g_type_ensure (g_power_profile_monitor_portal_get_type ());
      g_type_ensure (g_proxy_resolver_portal_get_type ());
#endif
#ifdef G_OS_WIN32
      g_type_ensure (g_win32_notification_backend_get_type ());
      g_type_ensure (_g_winhttp_vfs_get_type ());
      g_type_ensure (g_memory_monitor_win32_get_type ());
#endif
      g_type_ensure (_g_local_vfs_get_type ());
      g_type_ensure (_g_dummy_proxy_resolver_get_type ());
      g_type_ensure (_g_http_proxy_get_type ());
      g_type_ensure (_g_https_proxy_get_type ());
      g_type_ensure (_g_socks4a_proxy_get_type ());
      g_type_ensure (_g_socks4_proxy_get_type ());
      g_type_ensure (_g_socks5_proxy_get_type ());
      g_type_ensure (_g_dummy_tls_backend_get_type ());
      g_type_ensure (g_network_monitor_base_get_type ());
#ifdef HAVE_NETLINK
      g_type_ensure (_g_network_monitor_netlink_get_type ());
      g_type_ensure (_g_network_monitor_nm_get_type ());
#endif
#ifdef G_OS_WIN32
      g_type_ensure (_g_win32_network_monitor_get_type ());
#endif

      g_once_init_leave (&loaded_dirs, TRUE);
    }
}

static void
g_io_extension_point_free (GIOExtensionPoint *ep)
{
  g_free (ep->name);
  g_free (ep);
}

/**
 * g_io_extension_point_register:
 * @name: The name of the extension point
 *
 * Registers an extension point.
 *
 * Returns: (transfer none): the new #GIOExtensionPoint. This object is
 *    owned by GIO and should not be freed.
 */
GIOExtensionPoint *
g_io_extension_point_register (const char *name)
{
  GIOExtensionPoint *ep;
  
  G_LOCK (extension_points);
  if (extension_points == NULL)
    extension_points = g_hash_table_new_full (g_str_hash,
					      g_str_equal,
					      NULL,
					      (GDestroyNotify)g_io_extension_point_free);

  ep = g_hash_table_lookup (extension_points, name);
  if (ep != NULL)
    {
      G_UNLOCK (extension_points);
      return ep;
    }

  ep = g_new0 (GIOExtensionPoint, 1);
  ep->name = g_strdup (name);
  
  g_hash_table_insert (extension_points, ep->name, ep);
  
  G_UNLOCK (extension_points);

  return ep;
}

/**
 * g_io_extension_point_lookup:
 * @name: the name of the extension point
 *
 * Looks up an existing extension point.
 *
 * Returns: (transfer none): the #GIOExtensionPoint, or %NULL if there
 *    is no registered extension point with the given name.
 */
GIOExtensionPoint *
g_io_extension_point_lookup (const char *name)
{
  GIOExtensionPoint *ep;

  G_LOCK (extension_points);
  ep = NULL;
  if (extension_points != NULL)
    ep = g_hash_table_lookup (extension_points, name);
  
  G_UNLOCK (extension_points);

  return ep;
  
}

/**
 * g_io_extension_point_set_required_type:
 * @extension_point: a #GIOExtensionPoint
 * @type: the #GType to require
 *
 * Sets the required type for @extension_point to @type. 
 * All implementations must henceforth have this type.
 */
void
g_io_extension_point_set_required_type (GIOExtensionPoint *extension_point,
					GType              type)
{
  extension_point->required_type = type;
}

/**
 * g_io_extension_point_get_required_type:
 * @extension_point: a #GIOExtensionPoint
 *
 * Gets the required type for @extension_point.
 *
 * Returns: the #GType that all implementations must have, 
 *   or %G_TYPE_INVALID if the extension point has no required type
 */
GType
g_io_extension_point_get_required_type (GIOExtensionPoint *extension_point)
{
  return extension_point->required_type;
}

static void
lazy_load_modules (GIOExtensionPoint *extension_point)
{
  GIOModule *module;
  GList *l;

  for (l = extension_point->lazy_load_modules; l != NULL; l = l->next)
    {
      module = l->data;

      if (!module->initialized)
	{
	  if (g_type_module_use (G_TYPE_MODULE (module)))
	    g_type_module_unuse (G_TYPE_MODULE (module)); /* Unload */
	  else
	    g_printerr ("Failed to load module: %s\n",
			module->filename);
	}
    }
}

/**
 * g_io_extension_point_get_extensions:
 * @extension_point: a #GIOExtensionPoint
 *
 * Gets a list of all extensions that implement this extension point.
 * The list is sorted by priority, beginning with the highest priority.
 *
 * Returns: (element-type GIOExtension) (transfer none): a #GList of
 *     #GIOExtensions. The list is owned by GIO and should not be
 *     modified.
 */
GList *
g_io_extension_point_get_extensions (GIOExtensionPoint *extension_point)
{
  g_return_val_if_fail (extension_point != NULL, NULL);

  lazy_load_modules (extension_point);
  return extension_point->extensions;
}

/**
 * g_io_extension_point_get_extension_by_name:
 * @extension_point: a #GIOExtensionPoint
 * @name: the name of the extension to get
 *
 * Finds a #GIOExtension for an extension point by name.
 *
 * Returns: (transfer none): the #GIOExtension for @extension_point that has the
 *    given name, or %NULL if there is no extension with that name
 */
GIOExtension *
g_io_extension_point_get_extension_by_name (GIOExtensionPoint *extension_point,
					    const char        *name)
{
  GList *l;

  g_return_val_if_fail (name != NULL, NULL);

  lazy_load_modules (extension_point);
  for (l = extension_point->extensions; l != NULL; l = l->next)
    {
      GIOExtension *e = l->data;

      if (e->name != NULL &&
	  strcmp (e->name, name) == 0)
	return e;
    }
  
  return NULL;
}

static gint
extension_prio_compare (gconstpointer  a,
			gconstpointer  b)
{
  const GIOExtension *extension_a = a, *extension_b = b;

  if (extension_a->priority > extension_b->priority)
    return -1;

  if (extension_b->priority > extension_a->priority)
    return 1;

  return 0;
}

/**
 * g_io_extension_point_implement:
 * @extension_point_name: the name of the extension point
 * @type: the #GType to register as extension 
 * @extension_name: the name for the extension
 * @priority: the priority for the extension
 *
 * Registers @type as extension for the extension point with name
 * @extension_point_name. 
 *
 * If @type has already been registered as an extension for this 
 * extension point, the existing #GIOExtension object is returned.
 *
 * Returns: (transfer none): a #GIOExtension object for #GType
 */
GIOExtension *
g_io_extension_point_implement (const char *extension_point_name,
				GType       type,
				const char *extension_name,
				gint        priority)
{
  GIOExtensionPoint *extension_point;
  GIOExtension *extension;
  GList *l;

  g_return_val_if_fail (extension_point_name != NULL, NULL);

  extension_point = g_io_extension_point_lookup (extension_point_name);
  if (extension_point == NULL)
    {
      g_warning ("Tried to implement non-registered extension point %s", extension_point_name);
      return NULL;
    }
  
  if (extension_point->required_type != 0 &&
      !g_type_is_a (type, extension_point->required_type))
    {
      g_warning ("Tried to register an extension of the type %s to extension point %s. "
		 "Expected type is %s.",
		 g_type_name (type),
		 extension_point_name, 
		 g_type_name (extension_point->required_type));
      return NULL;
    }      

  /* It's safe to register the same type multiple times */
  for (l = extension_point->extensions; l != NULL; l = l->next)
    {
      extension = l->data;
      if (extension->type == type)
	return extension;
    }
  
  extension = g_slice_new0 (GIOExtension);
  extension->type = type;
  extension->name = g_strdup (extension_name);
  extension->priority = priority;
  
  extension_point->extensions = g_list_insert_sorted (extension_point->extensions,
						      extension, extension_prio_compare);
  
  return extension;
}

/**
 * g_io_extension_ref_class:
 * @extension: a #GIOExtension
 *
 * Gets a reference to the class for the type that is 
 * associated with @extension.
 *
 * Returns: (transfer full): the #GTypeClass for the type of @extension
 */
GTypeClass *
g_io_extension_ref_class (GIOExtension *extension)
{
  return g_type_class_ref (extension->type);
}

/**
 * g_io_extension_get_type:
 * @extension: a #GIOExtension
 *
 * Gets the type associated with @extension.
 *
 * Returns: the type of @extension
 */
GType
g_io_extension_get_type (GIOExtension *extension)
{
  return extension->type;
}

/**
 * g_io_extension_get_name:
 * @extension: a #GIOExtension
 *
 * Gets the name under which @extension was registered.
 *
 * Note that the same type may be registered as extension
 * for multiple extension points, under different names.
 *
 * Returns: the name of @extension.
 */
const char *
g_io_extension_get_name (GIOExtension *extension)
{
  return extension->name;
}

/**
 * g_io_extension_get_priority:
 * @extension: a #GIOExtension
 *
 * Gets the priority with which @extension was registered.
 *
 * Returns: the priority of @extension
 */
gint
g_io_extension_get_priority (GIOExtension *extension)
{
  return extension->priority;
}
