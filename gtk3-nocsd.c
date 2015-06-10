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

#include <gtk/gtk.h>
#include <gdk/gdk.h>

typedef void (*gtk_window_set_titlebar_t) (GtkWindow *window, GtkWidget *titlebar);
typedef gboolean (*gdk_screen_is_composited_t) (GdkScreen *screen);
typedef void (*gdk_window_set_decorations_t) (GdkWindow *window, GdkWMDecoration decorations);
typedef GType (*g_type_register_static_t) (GType parent_type, const gchar *type_name, const GTypeInfo *info, GTypeFlags flags);
typedef GType (*g_type_register_static_simple_t) (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags);
typedef void (*g_type_add_interface_static_t) (GType instance_type, GType interface_type, const GInterfaceInfo *info);
typedef void (*gtk_window_buildable_add_child_t) (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type);
typedef GObject* (*gtk_dialog_constructor_t) (GType type, guint n_construct_properties, GObjectConstructParam *construct_params);
typedef char *(*gtk_check_version_t) (guint required_major, guint required_minor, guint required_micro);

enum {
    GTK_LIBRARY,
    GDK_LIBRARY,
    GOBJECT_LIBRARY,
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

static const char *library_sonames[NUM_LIBRARIES] = {
    GTK_LIBRARY_SONAME,
    GDK_LIBRARY_SONAME,
    GOBJECT_LIBRARY_SONAME
};

static void * volatile library_handles[NUM_LIBRARIES] = {
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

// When set to true, this override gdk_screen_is_composited() and let it
// return FALSE temporarily. Then, client-side decoration (CSD) cannot be initialized.
volatile static __thread int disable_composite = 0;

static gboolean is_compatible_gtk_version() {
    /* Marking both as volatile here saves the trouble of caring about
     * memory barriers. */
    static volatile gboolean checked = FALSE;
    static volatile gboolean compatible = FALSE;
    static gtk_check_version_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gtk_check_version_t)find_orig_function(GTK_LIBRARY, "gtk_check_version");

    if(G_UNLIKELY(!checked)) {
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
            compatible = (orig_func(3, 10, 0) == NULL);
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

// This API exists since gtk+ 3.10
extern void gtk_window_set_titlebar (GtkWindow *window, GtkWidget *titlebar) {
    static gtk_window_set_titlebar_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gtk_window_set_titlebar_t)find_orig_function(GTK_LIBRARY, "gtk_window_set_titlebar");
    // printf("gtk_window_set_titlebar\n");
    ++disable_composite;
    orig_func(window, titlebar);
    if(window && titlebar)
        set_has_custom_title(window, TRUE);
    --disable_composite;
}

extern gboolean gdk_screen_is_composited (GdkScreen *screen) {
    static gdk_screen_is_composited_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gdk_screen_is_composited_t)find_orig_function(GDK_LIBRARY, "gdk_screen_is_composited");
    // printf("gdk_screen_is_composited: %d\n", disable_composite);
    if(is_compatible_gtk_version()) {
        if(disable_composite)
            return FALSE;
    }
    // g_assert(disable_composite);
    return orig_func(screen);
}

extern void gdk_window_set_decorations (GdkWindow *window, GdkWMDecoration decorations) {
    static gdk_window_set_decorations_t orig_func = NULL;
    if(!orig_func)
        orig_func = (gdk_window_set_decorations_t)find_orig_function(GDK_LIBRARY, "gdk_window_set_decorations");
    if(is_compatible_gtk_version()) {
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
    orig_func(window, decorations);
}

typedef void (*gtk_window_realize_t)(GtkWidget* widget);
static gtk_window_realize_t orig_gtk_window_realize = NULL;

static void fake_gtk_window_realize(GtkWidget* widget) {
    // printf("intercept gtk_window_realize()!!! %p, %s %d\n", widget, G_OBJECT_TYPE_NAME(widget), has_custom_title(widget));
    ++disable_composite;
    orig_gtk_window_realize(widget);
    --disable_composite;
    // printf("end gtk_window_realize()\n");
}

static gtk_dialog_constructor_t orig_gtk_dialog_constructor = NULL;
static GClassInitFunc orig_gtk_dialog_class_init = NULL;
static GType gtk_dialog_type = 0;

static GObject *fake_gtk_dialog_constructor (GType type, guint n_construct_properties, GObjectConstructParam *construct_params) {
    // printf("fake_gtk_dialog_constructor!! %d\n", disable_composite);
    ++disable_composite;
    GObject* obj = orig_gtk_dialog_constructor(type, n_construct_properties, construct_params);
    --disable_composite;
    // printf("end fake_gtk_dialog_constructor\n");
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
static GType gtk_window_type = 0;

static void fake_gtk_window_class_init (GtkWindowClass *klass, gpointer data) {
    orig_gtk_window_class_init(klass, data);
    GtkWidgetClass* widget_class = GTK_WIDGET_CLASS(klass);
    if(widget_class) {
        orig_gtk_window_realize = widget_class->realize;
        widget_class->realize = fake_gtk_window_realize;
    }
}

#if 0
extern GType g_type_register_static (GType parent_type, const gchar *type_name, const GTypeInfo *info, GTypeFlags flags) {
    static g_type_register_static_t orig_func = NULL;
    if(!orig_func)
        orig_func = (g_type_register_static_t)find_orig_function(GOBJECT_LIBRARY, "g_type_register_static");

    // printf("register %s\n", type_name);
    if(!orig_gtk_window_class_init) { // GtkWindow is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkWindow") == 0)) {
            // override GtkWindowClass
            GTypeInfo fake_info = *info;
            orig_gtk_window_class_init = info->class_init;
            fake_info.class_init = (GClassInitFunc)fake_gtk_window_class_init;
            gtk_window_type = orig_func(parent_type, type_name, &fake_info, flags);
            return gtk_window_type;
        }
    }
    return orig_func(parent_type, type_name, info, flags);
}
#endif

GType g_type_register_static_simple (GType parent_type, const gchar *type_name, guint class_size, GClassInitFunc class_init, guint instance_size, GInstanceInitFunc instance_init, GTypeFlags flags) {
    GType type;
    static g_type_register_static_simple_t orig_func = NULL;
    if(!orig_func)
        orig_func = (g_type_register_static_simple_t)find_orig_function(GOBJECT_LIBRARY, "g_type_register_static_simple");

    // printf("register simple %s\n", type_name);
    if(!orig_gtk_window_class_init) { // GtkWindow is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkWindow") == 0)) {
            // override GtkWindowClass
            orig_gtk_window_class_init = class_init;
            if(is_compatible_gtk_version()) {
                class_init = (GClassInitFunc)fake_gtk_window_class_init;
                gtk_window_type = orig_func(parent_type, type_name, class_size, class_init, instance_size, instance_init, flags);
                return gtk_window_type;
            }
        }
    }

    if(!orig_gtk_dialog_class_init) { // GtkDialog::constructor is not overriden
        if(type_name && G_UNLIKELY(strcmp(type_name, "GtkDialog") == 0)) {
            // override GtkDialogClass
            orig_gtk_dialog_class_init = class_init;
            if(is_compatible_gtk_version()) {
                class_init = (GClassInitFunc)fake_gtk_dialog_class_init;
                gtk_dialog_type = orig_func(parent_type, type_name, class_size, class_init, instance_size, instance_init, flags);
                return gtk_dialog_type;
            }
        }
    }
    type = orig_func(parent_type, type_name, class_size, class_init, instance_size, instance_init, flags);
    return type;
}

static gtk_window_buildable_add_child_t orig_gtk_window_buildable_add_child = NULL;

static void fake_gtk_window_buildable_add_child (GtkBuildable *buildable, GtkBuilder *builder, GObject *child, const gchar *type) {
    // setting a titelbar via GtkBuilder => disable compositing temporarily
    gboolean is_titlebar = (type && strcmp(type, "titlebar") == 0);
    if(is_titlebar) {
        // printf("gtk_window_buildable_add_child: %p, %s, %s, %s\n", buildable, G_OBJECT_TYPE_NAME(buildable), G_OBJECT_TYPE_NAME(child), type);
        ++disable_composite;
        if(child)
            set_has_custom_title(GTK_WINDOW(buildable), TRUE);
    }
    orig_gtk_window_buildable_add_child(buildable, builder, child, type);
    if(is_titlebar)
        --disable_composite;
    // printf("add child end\n");
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
    static g_type_add_interface_static_t orig_func = NULL;
    if(!orig_func)
        orig_func = (g_type_add_interface_static_t)find_orig_function(GOBJECT_LIBRARY, "g_type_add_interface_static");
    if(instance_type == gtk_window_type) {
        if(interface_type == GTK_TYPE_BUILDABLE) {
            // register GtkBuildable interface for GtkWindow class
            GInterfaceInfo fake_info = *info;
            orig_gtk_window_buildable_interface_init = info->interface_init;
            fake_info.interface_init = (GInterfaceInitFunc)fake_gtk_window_buildable_interface_init;
            orig_func(instance_type, interface_type, &fake_info);
            return;
        }
    }
    orig_func(instance_type, interface_type, info);
}
