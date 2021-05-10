/*
    gtk3-nocsd, a module used to disable GTK+3 client side decoration.

    Copyright (C) 2014  Hong Jen Yee (PCMan) <pcman.tw@gmail.com>

    http://lxqt.org/

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <pthread.h>
#include <errno.h>

#include <stdarg.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include <girffi.h>

#include <gobject/gvaluecollector.h>

typedef void (*gtk_window_buildable_add_child_t) (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type);
typedef GObject* (*gtk_dialog_constructor_t) (GType type, guint n_construct_properties, GObjectConstructParam *construct_params);
typedef char *(*gtk_check_version_t) (guint required_major, guint required_minor, guint required_micro);
typedef void (*gtk_header_bar_set_property_t) (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
typedef void (*gtk_header_bar_realize_t) (GtkWidget *widget);
typedef void (*gtk_header_bar_unrealize_t) (GtkWidget *widget);
typedef void (*gtk_header_bar_hierarchy_changed_t) (GtkWidget *widget, GtkWidget *previous_toplevel);

enum {
    GTK_LIBRARY,
    GDK_LIBRARY,
    GOBJECT_LIBRARY,
    GLIB_LIBRARY,
    GIREPOSITORY_LIBRARY,
    NUM_LIBRARIES
};

#ifndef GTK_LIBRARY_SONAME
#define GTK_LIBRARY_SONAME "libgtk-3.so.0"
#endif

#ifndef GDK_LIBRARY_SONAME
#define GDK_LIBRARY_SONAME "libgdk-3.so.0"
#endif

#ifndef GDK_LIBRARY_SONAME_V2
#define GDK_LIBRARY_SONAME_V2 "libgdk-x11-2.0.so.0"
#endif

#ifndef GOBJECT_LIBRARY_SONAME
#define GOBJECT_LIBRARY_SONAME "libgobject-2.0.so.0"
#endif

#ifndef GLIB_LIBRARY_SONAME
#define GLIB_LIBRARY_SONAME "libglib-2.0.so.0"
#endif

#ifndef GIREPOSITORY_LIBRARY_SONAME
#define GIREPOSITORY_LIBRARY_SONAME "libgirepository-1.0.so.1"
#endif

static const char *library_sonames[NUM_LIBRARIES] = {
    GTK_LIBRARY_SONAME,
    GDK_LIBRARY_SONAME,
    GOBJECT_LIBRARY_SONAME,
    GLIB_LIBRARY_SONAME,
    GIREPOSITORY_LIBRARY_SONAME
};

static const char *library_sonames_v2[NUM_LIBRARIES] = {
    NULL,
    GDK_LIBRARY_SONAME_V2,
    NULL,
    NULL,
    NULL
};

static void * volatile library_handles[NUM_LIBRARIES * 2] = {
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

static pthread_key_t key_tls;
static pthread_once_t key_tls_once = PTHREAD_ONCE_INIT;

/* Marking both as volatile here saves the trouble of caring about
 * memory barriers. */
static volatile gboolean is_compatible_gtk_version_cached = FALSE;
static volatile gboolean is_compatible_gtk_version_checked = FALSE;
static volatile int gtk2_active;

typedef struct gtk3_nocsd_tls_data_t {
  // When set to true, this override gdk_screen_is_composited() and let it
  // return FALSE temporarily. Then, client-side decoration (CSD) cannot be initialized.
  volatile int disable_composite;
  volatile int signal_capture_handler;
  volatile int fake_global_decoration_layout;
  volatile int in_info_collect;
  const char *volatile  signal_capture_name;
  volatile gpointer signal_capture_instance;
  volatile gpointer signal_capture_data;
  volatile GCallback signal_capture_callback;
} gtk3_nocsd_tls_data_t;

static gtk3_nocsd_tls_data_t *tls_data_location();
#define TLSD     (tls_data_location())

__attribute__((destructor)) static void cleanup_library_handles(void) {
    int i;

    for (i = 0; i < NUM_LIBRARIES * 2; i++) {
        if (library_handles[i])
            (void) dlclose(library_handles[i]);
    }
}

static void *find_orig_function(int try_gtk2, int library_id, const char *symbol) {
    void *handle;
    void *symptr;

    /* Ok, so in case both gtk2 + gtk3 are loaded, but we are using
     * gtk2, we don't know what RTLD_NEXT is going to choose - so we
     * must explicitly pick up the gtk2 versions... */
    if (try_gtk2 && gtk2_active)
        goto try_gtk2_version;

    /* This will work in most cases, and is completely thread-safe. */
    handle = dlsym(RTLD_NEXT, symbol);
    if (handle)
        return handle;

    /* dlsym(RTLD_NEXT, ...) will fail if the library using the symbol
     * is dlopen()d itself (e.g. a python module or similar), so what
     * we need to do is load the corresponding library ourselves and
     * look for the symbol there. We keep a reference to that library
     * so that we may close it again in a destructor function once we
     * are unloaded. dlopen()/dlclose() are refcounted, so we need to
     * use a mutex to protect dlopen(), otherwise we possibly could
     * take out more than one reference on the library and the cleanup
     * function wouldn't completely free it.
     *
     * Note that we use RTLD_NOLOAD, since we don't want to mask
     * problems if plugins aren't properly linked against gtk itself. */
    handle = library_handles[library_id];

    if (!handle) {
        static pthread_mutex_t handle_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&handle_mutex);
        /* we need to check again inside the mutex-protected block */
        handle = library_handles[library_id];
        if (!handle)
            handle = dlopen(library_sonames[library_id], RTLD_LAZY | RTLD_NOLOAD);
        if (handle)
            library_handles[library_id] = handle;
        pthread_mutex_unlock(&handle_mutex);
        if (!handle) {
            if (try_gtk2)
                goto try_gtk2_version;
            return NULL;
        }
    }

    symptr = dlsym(handle, symbol);
    if (symptr || !try_gtk2)
        return symptr;

try_gtk2_version:
    /* We overwrite some functions that are already available in GDK2.
     * So just trying to dlopen() the GDK3 library will not work here,
     * because GDK2 is going to be loaded instead. Therefore, retry
     * with the GDK2 library - but only do so if the try_gtk2 flag is
     * set, because we only want to do that for functions that were
     * already available in Gtk/GDK2. Functions that were introduced
     * in Gtk3 will not receive this treatment.
     *
     * We are very fortunate that the two relevant functions are
     * binary compatible between GDK2 and GDK3.
     */

    /* try_gtk2 should not be set for functions were we don't have a
     * Gtk2 variant of the library. So this should always hold.
     * Nevertheless, be paranoid. */
    if (!library_sonames_v2[library_id])
        return NULL;

    /* Same logic as above, but we use an offset in the library
     * handles and use the v2 soname. */
    handle = library_handles[NUM_LIBRARIES + library_id];
    if (!handle) {
        static pthread_mutex_t handle_v2_mutex = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&handle_v2_mutex);
        /* we need to check again inside the mutex-protected block */
        handle = library_handles[NUM_LIBRARIES + library_id];
        if (!handle)
            handle = dlopen(library_sonames_v2[library_id], RTLD_LAZY | RTLD_NOLOAD);
        if (handle)
            library_handles[NUM_LIBRARIES + library_id] = handle;
        pthread_mutex_unlock(&handle_v2_mutex);
        if (!handle)
            return NULL;
    }

    return dlsym(handle, symbol);
}

/* If a binary is compiled with ELF flag NOW (corresponding to RTLD_NOW),
 * but is not linked against gtk, if we use symbols from gtk the binary
 * they will fail to load. But we can't link this library against gtk3,
 * because we don't want to pull that in to every program and that
 * would also be incompatible with gtk2. Therefore, make sure we import
 * every function, not just those that we override, at runtime. */
#define HIDDEN_NAME2(a,b)   a ## b
#define NAME2(a,b)          HIDDEN_NAME2(a,b)
#define RUNTIME_IMPORT_FUNCTION(try_gtk2, library, function_name, return_type, arg_def_list, arg_use_list) \
    static return_type NAME2(rtlookup_, function_name) arg_def_list { \
        static return_type (*orig_func) arg_def_list = NULL;\
        if (!orig_func) \
            orig_func = find_orig_function(try_gtk2, library, #function_name); \
        return orig_func arg_use_list; \
    }

RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_css_provider_new, GtkCssProvider *, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_css_provider_load_from_data, void, (GtkCssProvider *provider, const gchar *data, gssize length, GError **error), (provider, data, length, error))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_window_new, GtkWidget *, (GtkWindowType type), (type))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_header_bar_new, GtkWidget *, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_window_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_header_bar_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_window_get_titlebar, GtkWidget *, (GtkWindow *window), (window))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_buildable_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_window_set_titlebar, void, (GtkWindow *window, GtkWidget *titlebar), (window, titlebar))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_header_bar_set_show_close_button, void, (GtkHeaderBar *bar, gboolean setting), (bar, setting))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_header_bar_set_decoration_layout, void, (GtkHeaderBar *bar, const gchar *layout), (bar, layout))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_header_bar_get_decoration_layout, const gchar *, (GtkHeaderBar *bar), (bar))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_style_context_add_class, void, (GtkStyleContext *context, const gchar *class_name), (context, class_name))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_style_context_remove_class, void, (GtkStyleContext *context, const gchar *class_name), (context, class_name))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_style_context_add_provider, void, (GtkStyleContext *context, GtkStyleProvider *provider, guint priority), (context, provider, priority))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_style_provider_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_destroy, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_mapped, gboolean, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_realized, gboolean, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_style_context, GtkStyleContext *, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_map, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_set_parent, void, (GtkWidget *widget, GtkWidget *parent), (widget, parent))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_unrealize, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_realize, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_settings, GtkSettings *, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GTK_LIBRARY, gtk_widget_get_toplevel, GtkWidget *, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(0, GDK_LIBRARY, gdk_window_get_user_data, void, (GdkWindow *window, gpointer *data), (window, data))
RUNTIME_IMPORT_FUNCTION(1, GDK_LIBRARY, gdk_screen_is_composited, gboolean, (GdkScreen *screen), (screen))
RUNTIME_IMPORT_FUNCTION(1, GDK_LIBRARY, gdk_window_set_decorations, void, (GdkWindow *window, GdkWMDecoration decorations), (window, decorations))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_get_data, gpointer, (GObject *object, const gchar *key), (object, key))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_set_data, void, (GObject *object, const gchar *key, gpointer data), (object, key, data))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_ref, gpointer, (gpointer object), (object))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_unref, void, (gpointer object), (object))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_check_class_cast, GTypeClass *, (GTypeClass *g_class, GType is_a_type), (g_class, is_a_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_check_instance_is_a, gboolean, (GTypeInstance *instance, GType iface_type), (instance, iface_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_check_instance_cast, GTypeInstance *, (GTypeInstance *instance, GType iface_type), (instance, iface_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_class_find_property, GParamSpec *, (GObjectClass *oclass, const gchar *property_name), (oclass, property_name))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_register_static_simple, GType, (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags), (parent_type, type_name, class_size, class_init, instance_size, instance_init, flags))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_add_interface_static, void, (GType instance_type, GType interface_type, const GInterfaceInfo *info), (instance_type, interface_type, info))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_add_instance_private, gint, (GType class_type, gsize private_size), (class_type, private_size))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_instance_get_private, gpointer, (GTypeInstance *instance, GType private_type), (instance, private_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_value_table_peek, GTypeValueTable *, (GType type), (type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_type_check_instance_is_fundamentally_a, gboolean, (GTypeInstance *instance, GType fundamental_type), (instance, fundamental_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_signal_connect_data, gulong, (gpointer instance, const gchar *detailed_signal, GCallback c_handler, gpointer data, GClosureNotify destroy_data, GConnectFlags connect_flags), (instance, detailed_signal, c_handler, data, destroy_data, connect_flags))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_signal_handlers_disconnect_matched, guint, (gpointer instance, GSignalMatchType mask, guint signal_id, GQuark detail, GClosure *closure, gpointer func, gpointer data), (instance, mask, signal_id, detail, closure, func, data))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_get_valist, void, (GObject *object, const gchar *first_property_name, va_list var_args), (object, first_property_name, var_args))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_object_get_property, void, (GObject *object, const gchar *property_name, GValue *value), (object, property_name, value))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_value_init, GValue *, (GValue *value, GType g_type), (value, g_type))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_value_unset, void, (GValue *value), (value))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_value_get_string, const gchar *, (const GValue *value), (value))
RUNTIME_IMPORT_FUNCTION(0, GOBJECT_LIBRARY, g_value_get_boolean, gboolean, (const GValue *value), (value))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_getenv, gchar *, (const char *name), (name))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_logv, void, (const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, va_list args), (log_domain, log_level, format, args))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_free, void, (gpointer mem), (mem))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_strdup, gchar *, (const gchar *str), (str))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_strfreev, void, (gchar **str_array), (str_array))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_strlcat, gsize, (gchar *dest, const gchar *src, gsize dest_size), (dest, src, dest_size))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_strlcpy, gsize, (gchar *dest, const gchar *src, gsize dest_size), (dest, src, dest_size))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_strsplit, gchar **, (const gchar *string, const gchar *delimiter, gint max_tokens), (string, delimiter, max_tokens))
RUNTIME_IMPORT_FUNCTION(0, GLIB_LIBRARY, g_assertion_message_expr, void, (const char *domain, const char *file, int line, const char *func, const char *expr), (domain, file, line, func, expr))
RUNTIME_IMPORT_FUNCTION(0, GIREPOSITORY_LIBRARY, g_function_info_prep_invoker, gboolean, (GIFunctionInfo *info, GIFunctionInvoker *invoker, GError **error), (info, invoker, error))

/* All methods that we want to overwrite are named orig_, all methods
 * that we just want to call (either directly or indirectrly)
 */
#define gtk_css_provider_new                             rtlookup_gtk_css_provider_new
#define gtk_css_provider_load_from_data                  rtlookup_gtk_css_provider_load_from_data
#define gtk_window_new                                   rtlookup_gtk_window_new
#define gtk_header_bar_new                               rtlookup_gtk_header_bar_new
#define gtk_window_get_type                              rtlookup_gtk_window_get_type
#define gtk_header_bar_get_type                          rtlookup_gtk_header_bar_get_type
#define gtk_widget_get_type                              rtlookup_gtk_widget_get_type
#define gtk_buildable_get_type                           rtlookup_gtk_buildable_get_type
#define gtk_window_get_titlebar                          rtlookup_gtk_window_get_titlebar
#define orig_gtk_window_set_titlebar                     rtlookup_gtk_window_set_titlebar
#define orig_gtk_header_bar_set_show_close_button        rtlookup_gtk_header_bar_set_show_close_button
#define orig_gtk_header_bar_set_decoration_layout        rtlookup_gtk_header_bar_set_decoration_layout
#define orig_gtk_header_bar_get_decoration_layout        rtlookup_gtk_header_bar_get_decoration_layout
#define gtk_style_context_add_class                      rtlookup_gtk_style_context_add_class
#define gtk_style_context_remove_class                   rtlookup_gtk_style_context_remove_class
#define gtk_style_context_add_provider                   rtlookup_gtk_style_context_add_provider
#define gtk_style_provider_get_type                      rtlookup_gtk_style_provider_get_type
#define gtk_widget_destroy                               rtlookup_gtk_widget_destroy
#define gtk_widget_get_mapped                            rtlookup_gtk_widget_get_mapped
#define gtk_widget_get_realized                          rtlookup_gtk_widget_get_realized
#define gtk_widget_get_style_context                     rtlookup_gtk_widget_get_style_context
#define gtk_widget_map                                   rtlookup_gtk_widget_map
#define gtk_widget_set_parent                            rtlookup_gtk_widget_set_parent
#define gtk_widget_unrealize                             rtlookup_gtk_widget_unrealize
#define gtk_widget_realize                               rtlookup_gtk_widget_realize
#define gdk_window_get_user_data                         rtlookup_gdk_window_get_user_data
#define orig_gdk_screen_is_composited                    rtlookup_gdk_screen_is_composited
#define orig_gdk_window_set_decorations                  rtlookup_gdk_window_set_decorations
#define g_object_get_data                                rtlookup_g_object_get_data
#define g_object_set_data                                rtlookup_g_object_set_data
#define g_type_check_class_cast                          rtlookup_g_type_check_class_cast
#define g_type_check_instance_is_a                       rtlookup_g_type_check_instance_is_a
#define g_type_check_instance_cast                       rtlookup_g_type_check_instance_cast
#define g_object_class_find_property                     rtlookup_g_object_class_find_property
#define g_object_get_valist                              rtlookup_g_object_get_valist
#define g_object_get_property                            rtlookup_g_object_get_property
#define g_object_ref                                     rtlookup_g_object_ref
#define g_object_unref                                   rtlookup_g_object_unref
#define g_value_init                                     rtlookup_g_value_init
#define g_value_unset                                    rtlookup_g_value_unset
#define g_value_get_string                               rtlookup_g_value_get_string
#define g_value_get_boolean                              rtlookup_g_value_get_boolean
#define orig_g_type_register_static_simple               rtlookup_g_type_register_static_simple
#define orig_g_type_add_interface_static                 rtlookup_g_type_add_interface_static
#define orig_g_type_add_instance_private                 rtlookup_g_type_add_instance_private
#define orig_g_signal_connect_data                       rtlookup_g_signal_connect_data
#define g_signal_handlers_disconnect_matched             rtlookup_g_signal_handlers_disconnect_matched
#define g_type_instance_get_private                      rtlookup_g_type_instance_get_private
#define g_type_value_table_peek                          rtlookup_g_type_value_table_peek
#define g_type_check_instance_is_fundamentally_a         rtlookup_g_type_check_instance_is_fundamentally_a
#define g_getenv                                         rtlookup_g_getenv
#define g_logv                                           rtlookup_g_logv
#define g_log                                            static_g_log
#define g_free                                           rtlookup_g_free
#define g_strdup                                         rtlookup_g_strdup
#define g_strfreev                                       rtlookup_g_strfreev
#define g_strlcat                                        rtlookup_g_strlcat
#define g_strlcpy                                        rtlookup_g_strlcpy
#define g_strsplit                                       rtlookup_g_strsplit
#define gtk_widget_get_settings                          rtlookup_gtk_widget_get_settings
#define gtk_widget_get_toplevel                          rtlookup_gtk_widget_get_toplevel
#define g_assertion_message_expr                         rtlookup_g_assertion_message_expr
#define orig_g_function_info_prep_invoker                rtlookup_g_function_info_prep_invoker

/* Forwarding of varadic functions is tricky. */
static void static_g_log(const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, ...)
{
    va_list args;
    va_start (args, format);
    g_logv (log_domain, log_level, format, args);
    va_end (args);
}

int check_gtk2_callback(struct dl_phdr_info *info, size_t size, void *pointer)
{
    ElfW(Half) n;

    if (G_UNLIKELY(strstr(info->dlpi_name, GDK_LIBRARY_SONAME_V2))) {
        for (n = 0; n < info->dlpi_phnum; n++) {
            uintptr_t start = (uintptr_t) (info->dlpi_addr + info->dlpi_phdr[n].p_vaddr);
            uintptr_t end   = start + (uintptr_t) info->dlpi_phdr[n].p_memsz;
            if ((uintptr_t) pointer >= start && (uintptr_t) pointer < end) {
                gtk2_active = 1;
                /* The gtk version check could have already been cached
                 * before we were able to determine that gtk2 is in
                 * use, so force this to FALSE. (Regardless of  the
                 * _checked value.) */
                is_compatible_gtk_version_cached = FALSE;
                return 0;
            }
        }
    }
    return 0;
}

static void detect_gtk2(void *pointer)
{
    if (gtk2_active)
        return;
    /* There is a corner case where a program with plugins loads
     * multiple plugins, some of which are linked against gtk2, while
     * others are linked against gtk3. If the gtk2 plugins are used,
     * this causes problems if we detect gtk3 just on the fact of
     * whether gtk3 is loaded. Hence we iterate over all loaded
     * libraries and if the pointer passed to us is within the memory
     * region of gtk2, we set a global flag. */
    dl_iterate_phdr(check_gtk2_callback, pointer);
}

static gboolean is_gtk_version_larger_or_equal2(guint major, guint minor, guint micro, int* gtk_loaded) {
    static gtk_check_version_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gtk_check_version_t)find_orig_function(0, GTK_LIBRARY, "gtk_check_version");

    /* We may have not been able to load the function IF a
     * gtk2-using plugin was loaded into a non-gtk application. In
     * that case, we don't want to do anything anyway, so just say
     * we aren't compatible.
     *
     * Note that if the application itself is using gtk2, RTLD_NEXT
     * will give us a reference to gtk_check_version. But since
     * that symbol is compatible with gtk3, this doesn't hurt.
     */
     if (orig_func) {
         if (gtk_loaded)
             *gtk_loaded = TRUE;
        return (orig_func(major, minor, micro) == NULL);
     } else {
         if (gtk_loaded)
             *gtk_loaded = FALSE;
        return FALSE;
     }
}

static gboolean is_gtk_version_larger_or_equal(guint major, guint minor, guint micro) {
    return is_gtk_version_larger_or_equal2(major, minor, micro, NULL);
}

static gboolean are_csd_disabled() {
    static volatile int csd_disabled = -1;
    if (csd_disabled == -1) {
        const gchar *csd_env;
        csd_env = g_getenv ("GTK_CSD");
        csd_disabled = csd_env != NULL && strcmp (csd_env, "1") != 0;
    }
    return csd_disabled;
}

static gboolean is_compatible_gtk_version() {
    int gtk_loaded = FALSE;

    if(G_UNLIKELY(!is_compatible_gtk_version_checked)) {
        if (gtk2_active) {
            is_compatible_gtk_version_cached = FALSE;
	} else if (!is_gtk_version_larger_or_equal2(3, 10, 0, &gtk_loaded)) {
            /* CSD was introduced there */
            is_compatible_gtk_version_cached = FALSE;
        } else {
            is_compatible_gtk_version_cached = TRUE;
        }
        /* If in a dynamical program (e.g. using python-gi) Glib is loaded before
         * Gtk, then the Gtk version check is executed before Gtk is even loaded,
         * returning FALSE and caching it. This will not disable CSD if Gtk is
         * loaded later. To circumvent this, cache the value only if we know that
         * Gtk is loaded. */
        if (gtk_loaded)
            is_compatible_gtk_version_checked = TRUE;
    }

    return is_compatible_gtk_version_cached;
}

static void set_has_custom_title(GtkWindow* window, gboolean set) {
    g_object_set_data(G_OBJECT(window), "custom_title", set ? GINT_TO_POINTER(1) : NULL);
}

static gboolean has_custom_title(GtkWindow* window) {
    return (gboolean)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(window), "custom_title"));
}

typedef void (*on_titlebar_title_notify_t) (GtkHeaderBar *titlebar, GParamSpec *pspec, GtkWindow *self);
typedef void (*update_window_buttons_t) (GtkHeaderBar *bar);
typedef gboolean (*window_state_changed_t) (GtkWidget *widget, GdkEventWindowState *event, gpointer data);

typedef struct gtk_window_private_info_t {
    gsize title_box_offset;
    on_titlebar_title_notify_t on_titlebar_title_notify;
} gtk_window_private_info_t;

typedef struct gtk_header_bar_private_info_t {
    gsize decoration_layout_offset;
    update_window_buttons_t update_window_buttons;
    window_state_changed_t window_state_changed;
} gtk_header_bar_private_info_t;

static GType gtk_window_type = -1;
static GType gtk_header_bar_type = -1;

static gtk_window_private_info_t gtk_window_private_info ();
static gtk_header_bar_private_info_t gtk_header_bar_private_info ();

static GtkStyleProvider *get_custom_css_provider ()
{
    static GtkStyleProvider *volatile provider = NULL;
    static pthread_mutex_t provider_mutex = PTHREAD_MUTEX_INITIALIZER;
    /* This CSS works around some design issues with the header bar
     * when used with gtk3-nocsd. We first disable the padding, else
     * the "drop shadow" (actually a bottom border) of the header bar
     * doesn't fill out the entire window and the edges look weird.
     * With CSD Gtk's CSS also disables the padding, so this should be
     * safe and relatively theme-agnostic. (There is additional padding
     * inside child widgets, this padding was only 1px or so anyway.)
     * Next, we make sure that the edges aren't rounded anymore, to
     * make sure that there are no small black pixels on the top left
     * and top right side of the window contents. This should also be
     * theme-agnostic.
     * IMPORTANT: The CSS selectors here have to have the same (or
     * higher) selectivity than the selectors used in the theme's CSS.
     * Otherwise the settings here will not take effect, even though
     * this CSS here has a higher priority. See
     * <https://www.w3.org/TR/selectors/#specificity> for details.
     */
    static const char *custom_css =
      "window > .titlebar:not(headerbar) {\n"
      "  padding: 0;\n"
      "  border-style: none;\n"
      "  border-color: transparent;\n"
      "}\n"
      ".background:not(.tiled):not(.maximized) .titlebar:backdrop,\n"
      ".background:not(.tiled):not(.maximized) .titlebar {\n"
      "  border-top-left-radius: 0;\n"
      "  border-top-right-radius: 0;\n"
      "}\n"
      "";

    if (G_UNLIKELY (provider == NULL)) {
        GtkCssProvider *new_provider;

        pthread_mutex_lock(&provider_mutex);
        if (provider != NULL) {
            /* Handle race condition. */
            pthread_mutex_unlock(&provider_mutex);
            return provider;
        }

        new_provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (new_provider, custom_css, -1, NULL);

        provider = GTK_STYLE_PROVIDER (new_provider);
        pthread_mutex_unlock(&provider_mutex);
    }

    return provider;
}

static void add_custom_css (GtkWidget *widget)
{
    GtkStyleContext *context = gtk_widget_get_style_context (widget);
    GtkStyleProvider *my_provider = get_custom_css_provider ();

    if (!context || !my_provider)
        return;

    /* Use a higher priority than SETTINGS, but lower than APPLICATION.
     * add_provider will make sure a given provider is not added twice.
     */
    gtk_style_context_add_provider (context, my_provider, GTK_STYLE_PROVIDER_PRIORITY_SETTINGS + 50);
}

// This API exists since gtk+ 3.10
extern void gtk_window_set_titlebar (GtkWindow *window, GtkWidget *titlebar) {
    if(!is_compatible_gtk_version() || !are_csd_disabled()) {
        orig_gtk_window_set_titlebar(window, titlebar);
        return;
    }
    if (titlebar && is_gtk_version_larger_or_equal (3, 16, 1)) {
        /* We have to reimplement gtk_window_set_titlebar ourselves, since
         * those Gtk versions don't support turning CSD off anymore.
         * This mainly does the same things as the original function
         * (consisting of adding the title bar widget + connecting signals),
         * but it will not enable CSD and not set the client_decorated flag
         * in the window private space. (We wouldn't know which bit it is
         * anyway.) */
        gtk_window_private_info_t private_info = gtk_window_private_info ();
        char *priv = G_TYPE_INSTANCE_GET_PRIVATE (window, gtk_window_type, char);
        gboolean was_mapped = FALSE;
        GtkWidget *widget = GTK_WIDGET (window);
        GtkWidget **title_box_ptr = NULL;

        /* Something went wrong, so just stick with the original
         * implementation. */
        if (private_info.title_box_offset == (gsize)-1 || private_info.title_box_offset == (gsize)-2 || !priv)
            goto orig_impl;

        title_box_ptr = (GtkWidget **) &priv[private_info.title_box_offset];

        if (!*title_box_ptr) {
            was_mapped = gtk_widget_get_mapped (widget);
            if (gtk_widget_get_realized (widget)) {
                g_warning ("gtk_window_set_titlebar() called on a realized window");
                gtk_widget_unrealize (widget);
            }
        }

        /* Remove any potential old title bar. We can't call
         * the static unset_titlebar() directly (not available),
         * so we call the full function; that shouldn't have
         * any side effects. */
        orig_gtk_window_set_titlebar (window, NULL);

        /* The solid-csd class is not removed when the titlebar
         * is unset in Gtk (it's probably a bug), so unset it
         * here explicitly, in case it's set. */
        gtk_style_context_remove_class (gtk_widget_get_style_context (widget), "solid-csd");

        /* We need to store the titlebar in priv->title_box,
         * which is where title_box_ptr points to. Then we
         * need to reparent the title bar and connect signals
         * if it's a GtkHeaderBar. Apart from CSD enablement,
         * this is what the original function boils down to.
         */
        *title_box_ptr = titlebar;
        gtk_widget_set_parent (*title_box_ptr, widget);
        if (GTK_IS_HEADER_BAR (titlebar)) {
            g_signal_connect (titlebar, "notify::title",
                        G_CALLBACK (private_info.on_titlebar_title_notify), window);
            private_info.on_titlebar_title_notify (GTK_HEADER_BAR (titlebar), NULL, window);
        }

        gtk_style_context_add_class (gtk_widget_get_style_context (titlebar),
                               GTK_STYLE_CLASS_TITLEBAR);

        add_custom_css (titlebar);

        if (was_mapped)
            gtk_widget_map (widget);

        return;
    }

orig_impl:
    ++(TLSD->disable_composite);
    orig_gtk_window_set_titlebar(window, titlebar);
    if(window && titlebar)
        set_has_custom_title(window, TRUE);
    --(TLSD->disable_composite);
}

static int _remove_buttons_from_layout (char *new_layout, const char *old_layout)
{
    gchar **tokens;
    gchar **t;
    int i, j, k;

    /* Assumptions: new_layout fits 256 bytes (including NUL), so make sure
     * that the old layout will fit */
    if (strlen (old_layout) > 255)
        return -1;

    new_layout[0] = '\0';

    tokens = g_strsplit (old_layout, ":", 2);
    if (tokens) {
        for (i = 0; i < 2; i++) {
            if (tokens[i] == NULL)
                break;
            if (i)
                g_strlcat (new_layout, ":", 256);
            t = g_strsplit (tokens[i], ",", -1);
            for (j = 0, k = 0; t[j]; j++) {
                /* We want to remove all standard window icons, while retaining
                 * custom stuff. */
                if (!strcmp (t[j], "icon") || !strcmp (t[j], "minimize") || !strcmp (t[j], "maximize") || !strcmp (t[j], "close"))
                    continue;
                if (k)
                    g_strlcat (new_layout, ",", 256);
                g_strlcat (new_layout, t[j], 256);
                k++;
            }

            g_strfreev (t);
        }
        g_strfreev (tokens);
    } else {
        g_strlcpy (new_layout, ":", 256);
    }

    return 0;
}

static void _gtk_header_bar_update_window_buttons (GtkHeaderBar *bar)
{
    gtk_header_bar_private_info_t info = gtk_header_bar_private_info ();
    char *priv = G_TYPE_INSTANCE_GET_PRIVATE (bar, gtk_header_bar_type, char);
    gchar **decoration_layout_ptr = NULL;
    gchar *orig_layout = NULL;
    gchar new_layout[256];
    int r;

    if (info.decoration_layout_offset == (gsize) -1 || info.decoration_layout_offset == (gsize) -2 || !priv) {
        return;
    }

    /* We shouldn't hit this case, but check nevertheless. */
    if (!is_compatible_gtk_version() || !are_csd_disabled()) {
        info.update_window_buttons (bar);
        return;
    }

    decoration_layout_ptr = (gchar **) &priv[info.decoration_layout_offset];
    if (*decoration_layout_ptr) {
        orig_layout = *decoration_layout_ptr;
        r = _remove_buttons_from_layout (new_layout, *decoration_layout_ptr);
        if (r == 0)
            *decoration_layout_ptr = new_layout;
    } else {
        TLSD->fake_global_decoration_layout = 1;
    }
    info.update_window_buttons (bar);
    if (*decoration_layout_ptr) {
        *decoration_layout_ptr = orig_layout;
    } else {
        TLSD->fake_global_decoration_layout = 0;
    }
}

static gboolean _gtk_header_bar_window_state_changed (GtkWidget *widget, GdkEventWindowState *event, gpointer data)
{
    gtk_header_bar_private_info_t info = gtk_header_bar_private_info ();
    GtkHeaderBar *bar = GTK_HEADER_BAR (data);
    gboolean ret;

    /* We can only be called if info.decoration_layout_offset is >= 0,
     * see hierarchy_changed, where this signal is connected, so this
     * shouldn't happen. If it does, though, just ignore the event,
     * it's certainly better than crashing with a segfault. */
    if (info.decoration_layout_offset == (gsize) -1 || info.decoration_layout_offset == (gsize) -2 || !info.window_state_changed) {
        return FALSE;
    }

    /* Technically, we don't actually need to run the current version of
     * window_state_changed, as the current Gtk+3 code does nothing more
     * than we do here. Unfortunately, we need to be future-proof, so
     * make call it anyway, and later override it again with our own code. */
    ret = info.window_state_changed (widget, event, data);
    if (!is_compatible_gtk_version() || !are_csd_disabled())
        return ret;

    if (event->changed_mask & (GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_TILED))
        _gtk_header_bar_update_window_buttons (bar);

    return ret;
}

extern void g_object_get (gpointer _object, const gchar *first_property_name, ...)
{
    GObject *object = _object;
    va_list var_args;
    const gchar *name;
    char new_layout[256];
    int r;

    if (!G_IS_OBJECT (_object))
        return;

    /* This is a really, really awful hack, because of the variable arguments
     * that g_object_get takes. At least Gtk+3 defines g_object_get_valist,
     * so we can default back to the valist original implementation if we
     * currently are not faking this for the decoration layout. Unfortunately,
     * there's no C way of forwarding to other varargs functions, so any other
     * preloaded library can't override the same function as we do here...
     * Fortunately, it's not very likely someone else wants to override
     * g_object_get(). */

    va_start (var_args, first_property_name);
    if (G_UNLIKELY (TLSD->fake_global_decoration_layout)) {
        name = first_property_name;
        while (name) {
            GValue value = G_VALUE_INIT;
            GParamSpec *spec = g_object_class_find_property (G_OBJECT_GET_CLASS (object), name);
            gchar *error;

            if (!spec)
                break;

            g_value_init (&value, spec->value_type);
            g_object_get_property (object, name, &value);

            if (G_UNLIKELY (strcmp (name, "gtk-decoration-layout") == 0)) {
                gchar **v = va_arg (var_args, gchar **);
                const gchar *s = g_value_get_string (&value);

                r = _remove_buttons_from_layout (new_layout, s);
                if (r == 0)
                    s = new_layout;
                *v = g_strdup (s);
            } else {
                G_VALUE_LCOPY (&value, var_args, 0, &error);
                if (error) {
                    g_warning ("%s: %s", "g_object_get_valist", error);
                    g_free (error);
                    g_value_unset (&value);
                    break;
                }
            }

            g_value_unset (&value);

            name = va_arg (var_args, gchar *);
        }
    } else {
        g_object_get_valist (object, first_property_name, var_args);
    }
    va_end (var_args);
}

extern void gtk_header_bar_set_show_close_button (GtkHeaderBar *bar, gboolean setting)
{
    /* Ancient Gtk+3 versions: we fake it via disabling show_close_button,
     * but that has adverse consequences, so in newer versions, where the
     * API is more complete, call our own implemnetation of u_w_b after
     * the original routine to perform some fixups. */
    if(is_compatible_gtk_version() && are_csd_disabled() && !is_gtk_version_larger_or_equal(3, 12, 0))
        setting = FALSE;
    orig_gtk_header_bar_set_show_close_button (bar, setting);
    if (is_compatible_gtk_version () && are_csd_disabled () && is_gtk_version_larger_or_equal (3, 12, 0))
        _gtk_header_bar_update_window_buttons (bar);
}

extern void gtk_header_bar_set_decoration_layout (GtkHeaderBar *bar, const gchar *layout)
{
    /* We need to call the original routine here, because it modifies the
     * private data structures. We fixup afterwards. */
    orig_gtk_header_bar_set_decoration_layout (bar, layout);
    if(is_compatible_gtk_version() && are_csd_disabled() && is_gtk_version_larger_or_equal(3, 12, 0)) {
        _gtk_header_bar_update_window_buttons (bar);
    }
}

extern gboolean gdk_screen_is_composited (GdkScreen *screen) {
    /* With Gtk+3 3.16.1+ we reimplement gtk_window_set_titlebar ourselves, hence
     * we don't want to re-use the compositing hack, especially since it causes
     * problems in newer Gtk versions. */
    if(is_compatible_gtk_version() && are_csd_disabled() && !is_gtk_version_larger_or_equal(3, 16, 1)) {
        if(TLSD->disable_composite)
            return FALSE;
    }
    return orig_gdk_screen_is_composited (screen);
}

extern void gdk_window_set_decorations (GdkWindow *window, GdkWMDecoration decorations) {
    if(is_compatible_gtk_version() && are_csd_disabled()) {
        if(decorations == GDK_DECOR_BORDER) {
            GtkWidget* widget = NULL;
            gdk_window_get_user_data(window, (void**)&widget);
            if(widget && GTK_IS_WINDOW(widget)) { // if this GdkWindow is associated with a GtkWindow
                // if this window has custom title (not using CSD), turn on all decorations
                if(has_custom_title(GTK_WINDOW(widget)))
                    decorations = GDK_DECOR_ALL;
            }
        }
    }
    orig_gdk_window_set_decorations (window, decorations);
}

typedef void (*gtk_window_realize_t)(GtkWidget* widget);
static gtk_window_realize_t orig_gtk_window_realize = NULL;

static void fake_gtk_window_realize(GtkWidget* widget) {
    ++(TLSD->disable_composite);
    orig_gtk_window_realize(widget);
    --(TLSD->disable_composite);
}

static gtk_dialog_constructor_t orig_gtk_dialog_constructor = NULL;
static GClassInitFunc orig_gtk_dialog_class_init = NULL;
static GType gtk_dialog_type = 0;

static GObject *fake_gtk_dialog_constructor (GType type, guint n_construct_properties, GObjectConstructParam *construct_params) {
    ++(TLSD->disable_composite);
    GObject* obj = orig_gtk_dialog_constructor(type, n_construct_properties, construct_params);
    --(TLSD->disable_composite);
    return obj;
}

static void fake_gtk_dialog_class_init (GtkDialogClass *klass, gpointer data) {
    orig_gtk_dialog_class_init(klass, data);
    // GDialogClass* dialog_class = GTK_DIALOG_CLASS(klass);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    if(object_class) {
        orig_gtk_dialog_constructor = object_class->constructor;
        object_class->constructor = fake_gtk_dialog_constructor;
    }
}


static GClassInitFunc orig_gtk_window_class_init = NULL;

static void fake_gtk_window_class_init (GtkWindowClass *klass, gpointer data) {
    orig_gtk_window_class_init(klass, data);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    if(widget_class) {
        orig_gtk_window_realize = widget_class->realize;
        widget_class->realize = fake_gtk_window_realize;
    }
}

static gtk_header_bar_set_property_t orig_gtk_header_bar_set_property = NULL;
static volatile int PROP_SHOW_CLOSE_BUTTON = -1;
static void fake_gtk_header_bar_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    /* We haven't determined the property yet... */
    if (G_UNLIKELY(PROP_SHOW_CLOSE_BUTTON == -1)) {
        GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(object), "show-close-button");
        if (spec)
            /* Technically, this is marked as internal in GParamSpec,
             * but it's the only way to access that trivially. It's
             * been stable in gobject for over a decade now. */
            PROP_SHOW_CLOSE_BUTTON = spec->param_id;
        else
            /* We couldn't find out, for some reason. The value -2
             * will never match a valid property id, so should be safe. */
            PROP_SHOW_CLOSE_BUTTON = -2;
    }

    /* In theory, we shouldn't need to override this, since
     * set_property in the gtk3 source code just calls that function,
     * but with active compiler optimization, an inline version of it
     * may be copied into set_propery, so we also need to override
     * this here. */
    if(G_UNLIKELY((int)prop_id == PROP_SHOW_CLOSE_BUTTON))
        gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (object), g_value_get_boolean (value));
    else
        orig_gtk_header_bar_set_property (object, prop_id, value, pspec);
}

static gtk_header_bar_realize_t orig_gtk_header_bar_realize = NULL;
static void fake_gtk_header_bar_realize (GtkWidget *widget)
{
    gtk_header_bar_private_info_t info;
    GtkSettings *settings;

    orig_gtk_header_bar_realize (widget);
    settings = gtk_widget_get_settings (widget);

    /* realize() is called from gtk_header_bar_private_info, so make sure
     * we special-case that. */
    if (G_UNLIKELY (TLSD->in_info_collect))
        return;

    info = gtk_header_bar_private_info ();
    if (info.decoration_layout_offset == (gsize) -1 || info.decoration_layout_offset == (gsize) -2)
        return;

    /* Replace signal handlers with our own */
    g_signal_handlers_disconnect_by_func (settings, info.update_window_buttons, widget);
    g_signal_connect_swapped (settings, "notify::gtk-shell-shows-app-menu", G_CALLBACK (_gtk_header_bar_update_window_buttons), widget);
    g_signal_connect_swapped (settings, "notify::gtk-decoration-layout", G_CALLBACK (_gtk_header_bar_update_window_buttons), widget);
    _gtk_header_bar_update_window_buttons (GTK_HEADER_BAR (widget));
}

static gtk_header_bar_unrealize_t orig_gtk_header_bar_unrealize = NULL;
static void fake_gtk_header_bar_unrealize (GtkWidget *widget)
{
    /* Disconnect our own signal handlers */
    GtkSettings *settings = gtk_widget_get_settings (widget);
    g_signal_handlers_disconnect_by_func (settings, _gtk_header_bar_update_window_buttons, widget);
    orig_gtk_header_bar_unrealize (widget);
}

static gtk_header_bar_hierarchy_changed_t orig_gtk_header_bar_hierarchy_changed = NULL;
static void fake_gtk_header_bar_hierarchy_changed (GtkWidget *widget, GtkWidget *previous_toplevel)
{
    gtk_header_bar_private_info_t info;
    GtkWidget *toplevel;
    GtkHeaderBar *bar = GTK_HEADER_BAR (widget);

    /* Older Gtk+3 versions didn't set this, so just ignore the event. */
    if (!orig_gtk_header_bar_hierarchy_changed)
        return;

    orig_gtk_header_bar_hierarchy_changed (widget, previous_toplevel);

    if (G_UNLIKELY (TLSD->in_info_collect))
        return;

    toplevel = gtk_widget_get_toplevel (widget);

    /* We can always do this. */
    if (previous_toplevel)
        g_signal_handlers_disconnect_by_func (previous_toplevel, _gtk_header_bar_window_state_changed, widget);

    info = gtk_header_bar_private_info ();

    if (info.decoration_layout_offset == (gsize) -1 && info.decoration_layout_offset == (gsize) -2)
        return;

    if (toplevel) {
        g_signal_handlers_disconnect_by_func (toplevel, info.window_state_changed, widget);
        g_signal_connect_after (toplevel, "window-state-event", G_CALLBACK (_gtk_header_bar_window_state_changed), widget);
    }

    _gtk_header_bar_update_window_buttons (bar);
}

static GClassInitFunc orig_gtk_header_bar_class_init = NULL;

static void fake_gtk_header_bar_class_init (GtkWindowClass *klass, gpointer data) {
    orig_gtk_header_bar_class_init(klass, data);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS (klass);
    if(object_class) {
        orig_gtk_header_bar_set_property = object_class->set_property;
        object_class->set_property = fake_gtk_header_bar_set_property;
    }
    if (widget_class) {
        orig_gtk_header_bar_realize = widget_class->realize;
        orig_gtk_header_bar_unrealize = widget_class->unrealize;
        orig_gtk_header_bar_hierarchy_changed = widget_class->hierarchy_changed;
        widget_class->realize = fake_gtk_header_bar_realize;
        widget_class->unrealize = fake_gtk_header_bar_unrealize;
        widget_class->hierarchy_changed = fake_gtk_header_bar_hierarchy_changed;
    }
}

static GInstanceInitFunc orig_gtk_shortcuts_window_init = NULL;

static void fake_gtk_shortcuts_window_init (GtkWindow *window, gpointer klass) {
    GtkHeaderBar *title_bar;

    orig_gtk_shortcuts_window_init ((GTypeInstance *) window, klass);
    /* call our own set_titlebar to make sure we disable CSDs
     * (the original Gtk function calls Gtk's internal set_titlebar) */
    title_bar = GTK_HEADER_BAR (gtk_window_get_titlebar (window));
    if (title_bar) {
        /* We need to take a reference out on the title_bar, because
         * unsetting it will unref() it indirectly via gtk_widget_unparent(),
         * which would otherwise call the destructor, because it's only
         * referenced once. */
        g_object_ref (title_bar);
        orig_gtk_window_set_titlebar (window, NULL);
        gtk_window_set_titlebar (window, GTK_WIDGET (title_bar));
        /* Drop our own reference (because it's not floating, and set_titlebar
         * calls ref_sink indirectly via set_parent) */
        g_object_unref (title_bar);
    }
}

GType g_type_register_static_simple (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags) {
    GType type;
    GType *save_type = NULL;

    if(!orig_gtk_window_class_init) { // GtkWindow is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkWindow") == 0)) {
            // override GtkWindowClass
            orig_gtk_window_class_init = class_init;
            detect_gtk2((void *) class_init);
            if(is_compatible_gtk_version() && are_csd_disabled()) {
                class_init = (GClassInitFunc)fake_gtk_window_class_init;
                save_type = &gtk_window_type;
                goto out;
            }
        }
    }

    if(!orig_gtk_dialog_class_init) { // GtkDialog::constructor is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkDialog") == 0)) {
            // override GtkDialogClass
            orig_gtk_dialog_class_init = class_init;
            detect_gtk2((void *) class_init);
            if(is_compatible_gtk_version() && are_csd_disabled()) {
                class_init = (GClassInitFunc)fake_gtk_dialog_class_init;
                save_type = &gtk_dialog_type;
                goto out;
            }
        }
    }

    if(!orig_gtk_header_bar_class_init) { // GtkHeaderBar::constructor is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkHeaderBar") == 0)) {
            // override GtkHeaderBarClass
            orig_gtk_header_bar_class_init = class_init;
            detect_gtk2((void *) class_init);
            if(is_compatible_gtk_version() && are_csd_disabled()) {
                class_init = (GClassInitFunc)fake_gtk_header_bar_class_init;
                save_type = &gtk_header_bar_type;
                goto out;
            }
        }
    }

    if(!orig_gtk_shortcuts_window_init) { // GtkShortcutsWindow::constructor is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkShortcutsWindow") == 0)) {
            // override GtkShortcutsWindowClass
            orig_gtk_shortcuts_window_init = instance_init;
            detect_gtk2((void *) instance_init);
            if(is_compatible_gtk_version() && are_csd_disabled()) {
                instance_init = (GInstanceInitFunc) fake_gtk_shortcuts_window_init;
                goto out;
            }
        }
    }

out:
    type = orig_g_type_register_static_simple (parent_type, type_name, class_size, class_init, instance_size, instance_init, flags);
    if (save_type)
        *save_type = type;
    return type;
}

static gtk_window_buildable_add_child_t orig_gtk_window_buildable_add_child = NULL;
static gtk_window_buildable_add_child_t orig_gtk_dialog_buildable_add_child = NULL;

static void fake_gtk_window_buildable_add_child (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type) {
    if (type && strcmp (type, "titlebar") == 0) {
        gtk_window_set_titlebar (GTK_WINDOW (buildable), GTK_WIDGET (child));
        return;
    }
    orig_gtk_window_buildable_add_child(buildable, builder, child, type);
}

static void fake_gtk_dialog_buildable_add_child (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type) {
    if (type && strcmp (type, "titlebar") == 0) {
        gtk_window_set_titlebar (GTK_WINDOW (buildable), GTK_WIDGET (child));
        return;
    }
    orig_gtk_dialog_buildable_add_child(buildable, builder, child, type);
}

static GInterfaceInitFunc orig_gtk_window_buildable_interface_init = NULL;
static GInterfaceInitFunc orig_gtk_dialog_buildable_interface_init = NULL;

static void fake_gtk_window_buildable_interface_init (GtkBuildableIface *iface, gpointer data)
{
    orig_gtk_window_buildable_interface_init(iface, data);
    orig_gtk_window_buildable_add_child = iface->add_child;
    iface->add_child = fake_gtk_window_buildable_add_child;
    // printf("intercept gtk_window_buildable_interface_init!!\n");
    // iface->set_buildable_property = gtk_window_buildable_set_buildable_property;
}

static void fake_gtk_dialog_buildable_interface_init (GtkBuildableIface *iface, gpointer data)
{
    orig_gtk_dialog_buildable_interface_init(iface, data);
    orig_gtk_dialog_buildable_add_child = iface->add_child;
    iface->add_child = fake_gtk_dialog_buildable_add_child;
}

void g_type_add_interface_static (GType instance_type, GType interface_type, const GInterfaceInfo *info) {
    if (info && info->interface_init)
        detect_gtk2((void *) info->interface_init);

    if(is_compatible_gtk_version() && are_csd_disabled() && (instance_type == gtk_window_type || instance_type == gtk_dialog_type)) {
        if(interface_type == GTK_TYPE_BUILDABLE) {
            // register GtkBuildable interface for GtkWindow/GtkDialog class
            GInterfaceInfo fake_info = *info;
            if (instance_type == gtk_window_type) {
                orig_gtk_window_buildable_interface_init = info->interface_init;
                fake_info.interface_init = (GInterfaceInitFunc)fake_gtk_window_buildable_interface_init;
            } else {
                orig_gtk_dialog_buildable_interface_init = info->interface_init;
                fake_info.interface_init = (GInterfaceInitFunc)fake_gtk_dialog_buildable_interface_init;
            }
            orig_g_type_add_interface_static (instance_type, interface_type, &fake_info);
            return;
        }
    }
    orig_g_type_add_interface_static (instance_type, interface_type, info);
}

static gsize gtk_window_private_size = 0;
static gint gtk_window_private_offset = 0;
static gsize gtk_header_bar_private_size = 0;
static gint gtk_header_bar_private_offset = 0;
gint g_type_add_instance_private (GType class_type, gsize private_size)
{
    if (G_UNLIKELY (class_type == gtk_window_type && gtk_window_private_size == 0)) {
        gtk_window_private_size = private_size;
        gtk_window_private_offset = orig_g_type_add_instance_private (class_type, private_size);
        return gtk_window_private_offset;
    } else if (G_UNLIKELY (class_type == gtk_header_bar_type && gtk_header_bar_private_size == 0)) {
        gtk_header_bar_private_size = private_size;
        gtk_header_bar_private_offset = orig_g_type_add_instance_private (class_type, private_size);
        return gtk_header_bar_private_offset;
    }
    return orig_g_type_add_instance_private (class_type, private_size);
}

gulong g_signal_connect_data (gpointer instance, const gchar *detailed_signal, GCallback c_handler, gpointer data, GClosureNotify destroy_data, GConnectFlags connect_flags)
{
    if (G_UNLIKELY (TLSD->signal_capture_handler)) {
        const char *name = TLSD->signal_capture_name;
        if (instance != NULL && TLSD->signal_capture_instance == instance && strcmp (detailed_signal, name) == 0)
            TLSD->signal_capture_callback = c_handler;
        else if (data != NULL && TLSD->signal_capture_data == data && strcmp (detailed_signal, name) == 0)
            TLSD->signal_capture_callback = c_handler;
    }
    return orig_g_signal_connect_data (instance, detailed_signal, c_handler, data, destroy_data, connect_flags);
}

static int find_unique_pointer_in_region (const char *haystack, gsize haystack_size, const void *needle)
{
    gsize i;
    int offset = -1;
    /* This is definitely not the most efficient algorithm, but
     * since this is only called twice per program, we're not
     * going to optimize it. */
    for (i = 0; i <= haystack_size - sizeof (needle); i++) {
        if (memcmp (&haystack[i], (void *) &needle, sizeof (needle)) == 0) {
            /* The pointer should only occur once in this memory
             * region - if not, then something's gone wrong. */
            if (offset != -1)
                return -2;
            offset = (int) i;
        }
    }
    return offset;
}

static gtk_window_private_info_t gtk_window_private_info ()
{
    static volatile gtk_window_private_info_t info = { (gsize) -1, NULL };
    if (G_UNLIKELY (info.title_box_offset == (gsize) -1)) {
        if (gtk_window_private_size != 0) {
            /* We have to detect the offset of where the title_box pointer
             * is stored in a GtkWindowPrivate object. This is required
             * because we need to change the pointer inside
             * GtkWindowPrivate causing any side effects (especially
             * without enabling CSD) - and the set_titlebar function will
             * enable CSD (in >= 3.16.1) if any non-NULL titlebar is set.
             * Therefore, we need to find the pointer offset within the
             * private data at runtime (it's not guaranteed to be stable
             * ABI-wise, so we can't just hard-code things, that would
             * lead to crashes). Furthermore, we need to know the address
             * of the static callback function that's used to process the
             * notify::title signal emitted by header bars.
             *
             * The basic algorithm is as follows: create a dummy GtkWindow,
             * create a dummy GtkHeaderBar, call the original
             * gtk_window_set_titlebar function and then look for the pointer
             * in the private data region of the GtkWindow. The size of the
             * region was recorded by us when the type was registered, so we
             * will stay within the proper memory region.
             */
            GtkWindow *dummy_window = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
            void *window_priv = NULL;
            GtkHeaderBar *dummy_bar = GTK_HEADER_BAR (gtk_header_bar_new ());
            int offset = -1;

            /* We're collecting information, so make sure all hacks
             * are NOOPS. */
            TLSD->in_info_collect = 1;

            if (!dummy_window || !dummy_bar) {
                g_warning ("libgtk3-nocsd: couldn't create dummy objects (GtkWindow, GtkHeaderBar) to determine this Gtk's runtime data structure layout");
                goto out;
            }

            /* First let's try to make sure the pointer is not present in
             * the memory region. */
            window_priv = G_TYPE_INSTANCE_GET_PRIVATE (dummy_window, gtk_window_type, void);
            offset = find_unique_pointer_in_region (window_priv, gtk_window_private_size, dummy_bar);
            /* Make sure the pointer isn't in there, else */
            if (offset != -1) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's runtime data structure layout: GtkWindow private structure already contained a pointer to GtkHeaderBar before setting title bar");
                goto out;
            }

            /* Set the title bar via the original title bar function. */
            TLSD->signal_capture_callback = NULL;
            TLSD->signal_capture_handler = 1;
            TLSD->signal_capture_name = "notify::title";
            TLSD->signal_capture_instance = dummy_bar;
            TLSD->signal_capture_data = NULL;
            orig_gtk_window_set_titlebar (dummy_window, GTK_WIDGET (dummy_bar));
            TLSD->signal_capture_handler = 0;
            TLSD->signal_capture_instance = NULL;
            TLSD->signal_capture_name = NULL;

            /* Now find the pointer in memory. */
            offset = find_unique_pointer_in_region (window_priv, gtk_window_private_size, dummy_bar);
            if (offset < 0) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's runtime data structure layout: GtkWindow private structure doesn't contain a pointer to GtkHeaderBar after setting title bar (error type %d)", -offset);
                goto out;
            }

            if (TLSD->signal_capture_callback == NULL) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's callback routine for GtkHeaderBar/GtkWindow interaction");
                goto out;
            }

            info.on_titlebar_title_notify = (on_titlebar_title_notify_t) TLSD->signal_capture_callback;
            info.title_box_offset = offset;
out:
            if (dummy_window) gtk_widget_destroy (GTK_WIDGET (dummy_window));
            else if (dummy_bar) gtk_widget_destroy (GTK_WIDGET (dummy_bar));

            TLSD->in_info_collect = 0;
        }
    }
    return info;
}

static gtk_header_bar_private_info_t gtk_header_bar_private_info ()
{
    static volatile gtk_header_bar_private_info_t info = { (gsize) -1, NULL };
    if (G_UNLIKELY (info.decoration_layout_offset == (gsize) -1)) {
        /* Was only introduced in Gtk+3 >= 3.12. Unlikely that someone is
         * still using such an old version, but be safe nevertheless. */
        if (G_UNLIKELY (!is_gtk_version_larger_or_equal(3, 12, 0))) {
            return info;
        }
        if (gtk_header_bar_private_size != 0) {
            /* We want to detect the offset of the pointer for the
             * decoration_layout string in the private structure, so we
             * create a header bar and set a decoration layout. As the
             * setter does g_strdup, we have to call the getter again,
             * because that will return the actual pointer that's stored.
             */
            GtkWindow *dummy_window = GTK_WINDOW (gtk_window_new (GTK_WINDOW_TOPLEVEL));
            GtkHeaderBar *dummy_bar = GTK_HEADER_BAR (gtk_header_bar_new ());
            void *header_bar_priv = NULL;
            const gchar *ptr = NULL;
            const gchar **ptr_in_priv;
            int offset = -1;
            gpointer ws_cb = NULL;

            /* We're collecting information, so make sure all hacks
             * are NOOPS. */
            TLSD->in_info_collect = 1;

            if (!dummy_bar || !dummy_window) {
                g_warning ("libgtk3-nocsd: couldn't create dummy object (GtkHeaderBar) to determine this Gtk's runtime data structure layout");
                goto out;
            }

            /* add it to our dummy window (we need a window here because
             * we want to realize the header bar, and that we can only
             * do if it's attached to a top-level window) */
            TLSD->signal_capture_callback = NULL;
            TLSD->signal_capture_handler = 1;
            TLSD->signal_capture_name = "window-state-event";
            TLSD->signal_capture_instance = dummy_window;
            TLSD->signal_capture_data = NULL;
            orig_gtk_window_set_titlebar (dummy_window, GTK_WIDGET (dummy_bar));
            TLSD->signal_capture_handler = 0;
            TLSD->signal_capture_instance = NULL;
            TLSD->signal_capture_name = NULL;
            ws_cb = TLSD->signal_capture_callback;

            header_bar_priv = G_TYPE_INSTANCE_GET_PRIVATE (dummy_bar, gtk_header_bar_type, void);

            orig_gtk_header_bar_set_decoration_layout (dummy_bar, "menu:close");
            ptr = orig_gtk_header_bar_get_decoration_layout (dummy_bar);
            offset = find_unique_pointer_in_region (header_bar_priv, gtk_header_bar_private_size, ptr);
            if (offset < 0) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's runtime data structure layout: GtkHeaderBar private structure doesn't contain a pointer to decoration_layout after setting it (error type %d)", -offset);
                goto out;
            }

            /* We now verify that the pointer is NULL */
            orig_gtk_header_bar_set_decoration_layout (dummy_bar, NULL);
            ptr_in_priv = (const gchar **) &((char *)header_bar_priv)[offset];
            if (*ptr_in_priv != NULL) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's runtime data structure layout: GtkHeaderBar's priv->decoration_layout pointer position sanity check failed (got pointer %p instead of NULL)", *ptr_in_priv);
                goto out;
            }

            /* realize the widget to capture the
             * _gtk_header_bar_update_window_buttons callback */
            TLSD->signal_capture_callback = NULL;
            TLSD->signal_capture_handler = 1;
            TLSD->signal_capture_name = "notify::gtk-decoration-layout";
            TLSD->signal_capture_instance = NULL;
            TLSD->signal_capture_data = dummy_bar;
            gtk_widget_realize (GTK_WIDGET (dummy_bar));
            TLSD->signal_capture_handler = 0;
            TLSD->signal_capture_data = NULL;
            TLSD->signal_capture_name = NULL;

            if (TLSD->signal_capture_callback == NULL) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's callback routine for GtkHeaderBar's button update");
                goto out;
            }

            info.decoration_layout_offset = offset;
            info.update_window_buttons = (update_window_buttons_t) TLSD->signal_capture_callback;
            /* Don't check ws_cb, it may be NULL, because older Gtk+3 versions didn't use that. */
            info.window_state_changed = (window_state_changed_t) ws_cb;
out:
            if (dummy_window) gtk_widget_destroy (GTK_WIDGET (dummy_window));
            else if (dummy_bar) gtk_widget_destroy (GTK_WIDGET (dummy_bar));

            TLSD->in_info_collect = 0;
        }
    }
    return info;
}

gboolean g_function_info_prep_invoker (GIFunctionInfo *info, GIFunctionInvoker *invoker, GError **error)
{
    static gpointer orig_set_titlebar = NULL, orig_set_show_close_button = NULL, orig_set_decoration_layout = NULL;
    gboolean result;

    if (!orig_set_titlebar)
        orig_set_titlebar = (gpointer) find_orig_function (0, GTK_LIBRARY, "gtk_window_set_titlebar");
    if (!orig_set_show_close_button)
        orig_set_show_close_button = (gpointer) find_orig_function (0, GTK_LIBRARY, "gtk_header_bar_set_show_close_button");
    if (!orig_set_decoration_layout)
        orig_set_decoration_layout = (gpointer) find_orig_function (0, GTK_LIBRARY, "gtk_header_bar_set_decoration_layout");

    result = orig_g_function_info_prep_invoker (info, invoker, error);
    if (result) {
        if (G_UNLIKELY (invoker->native_address == orig_set_titlebar))
            invoker->native_address = gtk_window_set_titlebar;
        if (G_UNLIKELY (invoker->native_address == orig_set_show_close_button))
            invoker->native_address = gtk_header_bar_set_show_close_button;
        if (G_UNLIKELY (invoker->native_address == orig_set_decoration_layout))
            invoker->native_address = gtk_header_bar_set_decoration_layout;
    }

    return result;
}

static void create_key_tls()
{
  int r;
  r = pthread_key_create (&key_tls, free);
  if (r < 0)
    g_error ("libgtk3-nocsd: unable to initialize TLS data: %s", strerror(errno));
}

static gtk3_nocsd_tls_data_t *tls_data_location()
{
  void *ptr;
  int r;

  (void) pthread_once (&key_tls_once, create_key_tls);
  if ((ptr = pthread_getspecific (key_tls)) == NULL) {
    ptr = calloc (1, sizeof (gtk3_nocsd_tls_data_t));
    if (!ptr)
      g_error ("libgtk3-nocsd: unable to initialize TLS data: %s", strerror(errno));
    r = pthread_setspecific (key_tls, ptr);
    if (r < 0)
      g_error ("libgtk3-nocsd: unable to initialize TLS data: %s", strerror(errno));
  }

  return (gtk3_nocsd_tls_data_t *) ptr;
}
