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
#include <unistd.h>
#include <string.h>

#include <pthread.h>

#include <stdarg.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>

typedef void (*gtk_window_buildable_add_child_t) (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type);
typedef GObject* (*gtk_dialog_constructor_t) (GType type, guint n_construct_properties, GObjectConstructParam *construct_params);
typedef char *(*gtk_check_version_t) (guint required_major, guint required_minor, guint required_micro);
typedef void (*gtk_header_bar_set_property_t) (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);

enum {
    GTK_LIBRARY,
    GDK_LIBRARY,
    GOBJECT_LIBRARY,
    GLIB_LIBRARY,
    NUM_LIBRARIES
};

#ifndef GTK_LIBRARY_SONAME
#define GTK_LIBRARY_SONAME "libgtk-3.so.0"
#endif

#ifndef GDK_LIBRARY_SONAME
#define GDK_LIBRARY_SONAME "libgdk-3.so.0"
#endif

#ifndef GOBJECT_LIBRARY_SONAME
#define GOBJECT_LIBRARY_SONAME "libgobject-2.0.so"
#endif

#ifndef GLIB_LIBRARY_SONAME
#define GLIB_LIBRARY_SONAME "libglib-2.0.so"
#endif

static const char *library_sonames[NUM_LIBRARIES] = {
    GTK_LIBRARY_SONAME,
    GDK_LIBRARY_SONAME,
    GOBJECT_LIBRARY_SONAME,
    GLIB_LIBRARY_SONAME
};

static void * volatile library_handles[NUM_LIBRARIES] = {
    NULL,
    NULL,
    NULL,
    NULL
};

__attribute__((destructor)) static void cleanup_library_handles(void) {
    int i;

    for (i = 0; i < NUM_LIBRARIES; i++) {
        if (library_handles[i])
            (void) dlclose(library_handles[i]);
    }
}

static void *find_orig_function(int library_id, const char *symbol) {
    void *handle;

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
#define RUNTIME_IMPORT_FUNCTION(library, function_name, return_type, arg_def_list, arg_use_list) \
    static return_type NAME2(rtlookup_, function_name) arg_def_list { \
        static return_type (*orig_func) arg_def_list = NULL;\
        if (!orig_func) \
            orig_func = find_orig_function(library, #function_name); \
        return orig_func arg_use_list; \
    }

RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_window_new, GtkWidget *, (GtkWindowType type), (type))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_header_bar_new, GtkWidget *, (), ())
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_window_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_header_bar_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_buildable_get_type, GType, (), ())
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_window_set_titlebar, void, (GtkWindow *window, GtkWidget *titlebar), (window, titlebar))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_header_bar_set_show_close_button, void, (GtkHeaderBar *bar, gboolean setting), (bar, setting))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_style_context_add_class, void, (GtkStyleContext *context, const gchar *class_name), (context, class_name))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_style_context_remove_class, void, (GtkStyleContext *context, const gchar *class_name), (context, class_name))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_destroy, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_get_mapped, gboolean, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_get_realized, gboolean, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_get_style_context, GtkStyleContext *, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_map, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_set_parent, void, (GtkWidget *widget, GtkWidget *parent), (widget, parent))
RUNTIME_IMPORT_FUNCTION(GTK_LIBRARY, gtk_widget_unrealize, void, (GtkWidget *widget), (widget))
RUNTIME_IMPORT_FUNCTION(GDK_LIBRARY, gdk_window_get_user_data, void, (GdkWindow *window, gpointer *data), (window, data))
RUNTIME_IMPORT_FUNCTION(GDK_LIBRARY, gdk_screen_is_composited, gboolean, (GdkScreen *screen), (screen))
RUNTIME_IMPORT_FUNCTION(GDK_LIBRARY, gdk_window_set_decorations, void, (GdkWindow *window, GdkWMDecoration decorations), (window, decorations))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_object_get_data, gpointer, (GObject *object, const gchar *key), (object, key))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_object_set_data, void, (GObject *object, const gchar *key, gpointer data), (object, key, data))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_check_class_cast, GTypeClass *, (GTypeClass *g_class, GType is_a_type), (g_class, is_a_type))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_check_instance_is_a, gboolean, (GTypeInstance *instance, GType iface_type), (instance, iface_type))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_check_instance_cast, GTypeInstance *, (GTypeInstance *instance, GType iface_type), (instance, iface_type))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_object_class_find_property, GParamSpec *, (GObjectClass *oclass, const gchar *property_name), (oclass, property_name))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_register_static_simple, GType, (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags), (parent_type, type_name, class_size, class_init, instance_size, instance_init, flags))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_add_interface_static, void, (GType instance_type, GType interface_type, const GInterfaceInfo *info), (instance_type, interface_type, info))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_add_instance_private, gint, (GType class_type, gsize private_size), (class_type, private_size))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_type_instance_get_private, gpointer, (GTypeInstance *instance, GType private_type), (instance, private_type))
RUNTIME_IMPORT_FUNCTION(GOBJECT_LIBRARY, g_signal_connect_data, gulong, (gpointer instance, const gchar *detailed_signal, GCallback c_handler, gpointer data, GClosureNotify destroy_data, GConnectFlags connect_flags), (instance, detailed_signal, c_handler, data, destroy_data, connect_flags))
RUNTIME_IMPORT_FUNCTION(GLIB_LIBRARY, g_getenv, gchar *, (const char *name), (name))
RUNTIME_IMPORT_FUNCTION(GLIB_LIBRARY, g_logv, void, (const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, va_list args), (log_domain, log_level, format, args))

/* All methods that we want to overwrite are named orig_, all methods
 * that we just want to call (either directly or indirectrly)
 */
#define gtk_window_new                                   rtlookup_gtk_window_new
#define gtk_header_bar_new                               rtlookup_gtk_header_bar_new
#define gtk_window_get_type                              rtlookup_gtk_window_get_type
#define gtk_header_bar_get_type                          rtlookup_gtk_header_bar_get_type
#define gtk_widget_get_type                              rtlookup_gtk_widget_get_type
#define gtk_buildable_get_type                           rtlookup_gtk_buildable_get_type
#define orig_gtk_window_set_titlebar                     rtlookup_gtk_window_set_titlebar
#define orig_gtk_header_bar_set_show_close_button        rtlookup_gtk_header_bar_set_show_close_button
#define gtk_style_context_add_class                      rtlookup_gtk_style_context_add_class
#define gtk_style_context_remove_class                   rtlookup_gtk_style_context_remove_class
#define gtk_widget_destroy                               rtlookup_gtk_widget_destroy
#define gtk_widget_get_mapped                            rtlookup_gtk_widget_get_mapped
#define gtk_widget_get_realized                          rtlookup_gtk_widget_get_realized
#define gtk_widget_get_style_context                     rtlookup_gtk_widget_get_style_context
#define gtk_widget_map                                   rtlookup_gtk_widget_map
#define gtk_widget_set_parent                            rtlookup_gtk_widget_set_parent
#define gtk_widget_unrealize                             rtlookup_gtk_widget_unrealize
#define gdk_window_get_user_data                         rtlookup_gdk_window_get_user_data
#define orig_gdk_screen_is_composited                    rtlookup_gdk_screen_is_composited
#define orig_gdk_window_set_decorations                  rtlookup_gdk_window_set_decorations
#define g_object_get_data                                rtlookup_g_object_get_data
#define g_object_set_data                                rtlookup_g_object_set_data
#define g_type_check_class_cast                          rtlookup_g_type_check_class_cast
#define g_type_check_instance_is_a                       rtlookup_g_type_check_instance_is_a
#define g_type_check_instance_cast                       rtlookup_g_type_check_instance_cast
#define g_object_class_find_property                     rtlookup_g_object_class_find_property
#define orig_g_type_register_static_simple               rtlookup_g_type_register_static_simple
#define orig_g_type_add_interface_static                 rtlookup_g_type_add_interface_static
#define orig_g_type_add_instance_private                 rtlookup_g_type_add_instance_private
#define orig_g_signal_connect_data                       rtlookup_g_signal_connect_data
#define g_type_instance_get_private                      rtlookup_g_type_instance_get_private
#define g_getenv                                         rtlookup_g_getenv
#define g_logv                                           rtlookup_g_logv
#define g_log                                            static_g_log

/* Forwarding of varadic functions is tricky. */
static void static_g_log(const gchar *log_domain, GLogLevelFlags log_level, const gchar *format, ...)
{
    va_list args;
    va_start (args, format);
    g_logv (log_domain, log_level, format, args);
    va_end (args);
}

// When set to true, this override gdk_screen_is_composited() and let it
// return FALSE temporarily. Then, client-side decoration (CSD) cannot be initialized.
volatile static __thread int disable_composite = 0;

static gboolean is_gtk_version_larger_or_equal(guint major, guint minor, guint micro) {
    static gtk_check_version_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gtk_check_version_t)find_orig_function(GTK_LIBRARY, "gtk_check_version");

    /* We may have not been able to load the function IF a
     * gtk2-using plugin was loaded into a non-gtk application. In
     * that case, we don't want to do anything anyway, so just say
     * we aren't compatible.
     *
     * Note that if the application itself is using gtk2, RTLD_NEXT
     * will give us a reference to gtk_check_version. But since
     * that symbol is compatible with gtk3, this doesn't hurt.
     */
     if (orig_func)
        return (orig_func(major, minor, micro) == NULL);
    return FALSE;
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
    /* Marking both as volatile here saves the trouble of caring about
     * memory barriers. */
    static volatile gboolean checked = FALSE;
    static volatile gboolean compatible = FALSE;

    if(G_UNLIKELY(!checked)) {
        if (!is_gtk_version_larger_or_equal(3, 10, 0)) {
            /* CSD was introduced there */
            compatible = FALSE;
        } else {
            compatible = TRUE;
        }
        checked = TRUE;
    }

    return compatible;
}

static void set_has_custom_title(GtkWindow* window, gboolean set) {
    g_object_set_data(G_OBJECT(window), "custom_title", set ? GINT_TO_POINTER(1) : NULL);
}

static gboolean has_custom_title(GtkWindow* window) {
    return (gboolean)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(window), "custom_title"));
}

typedef void (*on_titlebar_title_notify_t) (GtkHeaderBar *titlebar, GParamSpec *pspec, GtkWindow *self);

typedef struct gtk_window_private_info_t {
    gsize title_box_offset;
    on_titlebar_title_notify_t on_titlebar_title_notify;
} gtk_window_private_info_t;

static GType gtk_window_type = 0;

static gtk_window_private_info_t gtk_window_private_info ();

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
        if (private_info.title_box_offset < 0 || !priv)
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

        if (was_mapped)
            gtk_widget_map (widget);

        return;
    }

orig_impl:
    ++disable_composite;
    orig_gtk_window_set_titlebar(window, titlebar);
    if(window && titlebar)
        set_has_custom_title(window, TRUE);
    --disable_composite;
}

extern void gtk_header_bar_set_show_close_button (GtkHeaderBar *bar, gboolean setting)
{
    if(is_compatible_gtk_version() && are_csd_disabled())
        setting = FALSE;
    orig_gtk_header_bar_set_show_close_button (bar, setting);
}

extern gboolean gdk_screen_is_composited (GdkScreen *screen) {
    if(is_compatible_gtk_version() && are_csd_disabled()) {
        if(disable_composite)
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
    ++disable_composite;
    orig_gtk_window_realize(widget);
    --disable_composite;
}

static gtk_dialog_constructor_t orig_gtk_dialog_constructor = NULL;
static GClassInitFunc orig_gtk_dialog_class_init = NULL;
static GType gtk_dialog_type = 0;

static GObject *fake_gtk_dialog_constructor (GType type, guint n_construct_properties, GObjectConstructParam *construct_params) {
    ++disable_composite;
    GObject* obj = orig_gtk_dialog_constructor(type, n_construct_properties, construct_params);
    --disable_composite;
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
        gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (object), FALSE);
    else
        orig_gtk_header_bar_set_property (object, prop_id, value, pspec);
}

static GClassInitFunc orig_gtk_header_bar_class_init = NULL;
static GType gtk_header_bar_type = -1;

static void fake_gtk_header_bar_class_init (GtkWindowClass *klass, gpointer data) {
    orig_gtk_header_bar_class_init(klass, data);
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    if(object_class) {
        orig_gtk_header_bar_set_property = object_class->set_property;
        object_class->set_property = fake_gtk_header_bar_set_property;
    }
}

GType g_type_register_static_simple (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags) {
    GType type;
    GType *save_type = NULL;

    if(!orig_gtk_window_class_init) { // GtkWindow is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkWindow") == 0)) {
            // override GtkWindowClass
            orig_gtk_window_class_init = class_init;
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
            if(is_compatible_gtk_version() && are_csd_disabled()) {
                class_init = (GClassInitFunc)fake_gtk_header_bar_class_init;
                save_type = &gtk_header_bar_type;
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

static void fake_gtk_window_buildable_add_child (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type) {
    if (type && strcmp (type, "titlebar") == 0) {
        gtk_window_set_titlebar (GTK_WINDOW (buildable), GTK_WIDGET (child));
        return;
    }
    orig_gtk_window_buildable_add_child(buildable, builder, child, type);
}

static GInterfaceInitFunc orig_gtk_window_buildable_interface_init = NULL;

static void fake_gtk_window_buildable_interface_init (GtkBuildableIface *iface, gpointer data)
{
    orig_gtk_window_buildable_interface_init(iface, data);
    orig_gtk_window_buildable_add_child = iface->add_child;
    iface->add_child = fake_gtk_window_buildable_add_child;
    // printf("intercept gtk_window_buildable_interface_init!!\n");
    // iface->set_buildable_property = gtk_window_buildable_set_buildable_property;
}

void g_type_add_interface_static (GType instance_type, GType interface_type, const GInterfaceInfo *info) {
    if(is_compatible_gtk_version() && are_csd_disabled() && instance_type == gtk_window_type) {
        if(interface_type == GTK_TYPE_BUILDABLE) {
            // register GtkBuildable interface for GtkWindow class
            GInterfaceInfo fake_info = *info;
            orig_gtk_window_buildable_interface_init = info->interface_init;
            fake_info.interface_init = (GInterfaceInitFunc)fake_gtk_window_buildable_interface_init;
            orig_g_type_add_interface_static (instance_type, interface_type, &fake_info);
            return;
        }
    }
    orig_g_type_add_interface_static (instance_type, interface_type, info);
}

static gsize gtk_window_private_size = 0;
static gint gtk_window_private_offset = 0;
gint g_type_add_instance_private (GType class_type, gsize private_size)
{
    if (G_UNLIKELY (class_type == gtk_window_type && gtk_window_private_size == 0)) {
        gtk_window_private_size = private_size;
        gtk_window_private_offset = orig_g_type_add_instance_private (class_type, private_size);
        return gtk_window_private_offset;
    }
    return orig_g_type_add_instance_private (class_type, private_size);
}

volatile static __thread int signal_capture_handler = 0;
volatile static __thread gpointer signal_capture_instance = NULL;
volatile static __thread GCallback signal_capture_callback = NULL;

extern gulong g_signal_connect_data (gpointer instance, const gchar *detailed_signal, GCallback c_handler, gpointer data, GClosureNotify destroy_data, GConnectFlags connect_flags)
{
    if (G_UNLIKELY (signal_capture_handler)) {
        if (signal_capture_instance == instance && strcmp (detailed_signal, "notify::title") == 0)
            signal_capture_callback = c_handler;
    }
    return orig_g_signal_connect_data (instance, detailed_signal, c_handler, data, destroy_data, connect_flags);
}

static int find_unique_pointer_in_region (const char *haystack, gsize haystack_size, void *needle)
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
            signal_capture_callback = NULL;
            signal_capture_handler = 1;
            signal_capture_instance = dummy_bar;
            orig_gtk_window_set_titlebar (dummy_window, GTK_WIDGET (dummy_bar));
            signal_capture_handler = 0;
            signal_capture_instance = NULL;

            /* Now find the pointer in memory. */
            offset = find_unique_pointer_in_region (window_priv, gtk_window_private_size, dummy_bar);
            if (offset < 0) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's runtime data structure layout: GtkWindow private structure doesn't contain a pointer to GtkHeaderBar after setting title bar (error type %d)", -offset);
                goto out;
            }

            if (signal_capture_callback == NULL) {
                g_warning ("libgtk3-nocsd: error trying to determine this Gtk's callback routine for GtkHeaderBar/GtkWindow interaction");
                goto out;
            }

            info.on_titlebar_title_notify = (on_titlebar_title_notify_t) signal_capture_callback;
            info.title_box_offset = offset;
out:
            if (dummy_window) gtk_widget_destroy (GTK_WIDGET (dummy_window));
        }
    }
    return info;
}
