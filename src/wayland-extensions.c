/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include <stdint.h>
#include <string.h>

#include <gtk/gtk.h>

#include <gdk/gdkwayland.h>
#include <wayland-client-core.h>
#include <glib-unix.h>
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"

#include "wayland-extensions.h"

static void *
registry_bind_gtk(GtkWidget *widget,
                  uint32_t name,
                  const struct wl_interface *interface,
                  uint32_t version)
{
    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    struct wl_display *display;
    struct wl_registry *registry;

    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        return NULL;
    }

    display = gdk_wayland_display_get_wl_display(gdk_display);
    registry = wl_display_get_registry(display);

    return wl_registry_bind(registry, name, interface, version);
}

static void
registry_handle_global(void *data,
                       struct wl_registry *registry,
                       uint32_t name,
                       const char *interface,
                       uint32_t version)
{
    GtkWidget *widget = GTK_WIDGET(data);

    if (g_strcmp0(interface, "zwp_relative_pointer_manager_v1") == 0) {
        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
        relative_pointer_manager = registry_bind_gtk(widget, name,
                                                     &zwp_relative_pointer_manager_v1_interface,
                                                     1);
        g_object_set_data_full(G_OBJECT(widget),
                               "zwp_relative_pointer_manager_v1",
                               relative_pointer_manager,
                               (GDestroyNotify)zwp_relative_pointer_manager_v1_destroy);
        g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name", GUINT_TO_POINTER(name));
    } else if (g_strcmp0(interface, "zwp_pointer_constraints_v1") == 0) {
        struct zwp_pointer_constraints_v1 *pointer_constraints;
        pointer_constraints = registry_bind_gtk(widget, name,
                                                &zwp_pointer_constraints_v1_interface,
                                                1);
        g_object_set_data_full(G_OBJECT(widget),
                               "zwp_pointer_constraints_v1",
                               pointer_constraints,
                               (GDestroyNotify)zwp_pointer_constraints_v1_destroy);
        g_object_set_data(G_OBJECT(widget), "zwp_pointer_constraints_v1_name", GUINT_TO_POINTER(name));
    }
}

static void
registry_handle_global_remove(void *data,
                              struct wl_registry *registry,
                              uint32_t name)
{
    GtkWidget *widget = GTK_WIDGET(data);

    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    uint32_t relative_pointer_manager_name = 0;
    relative_pointer_manager = g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_manager_v1");
    relative_pointer_manager_name = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name"));
    if (relative_pointer_manager && relative_pointer_manager_name == name) {
        g_object_set_data_full(G_OBJECT(widget), "zwp_relative_pointer_manager_v1", NULL, NULL);
        g_object_steal_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name");
    }

    struct zwp_pointer_constraints_v1 *pointer_constraints;
    uint32_t pointer_constraints_name = 0;
    pointer_constraints = g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1");
    pointer_constraints_name = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1_name"));
    if (pointer_constraints && pointer_constraints_name == name) {
        g_object_set_data_full(G_OBJECT(widget), "zwp_pointer_constraints_v1", NULL, NULL);
        g_object_steal_data(G_OBJECT(widget), "zwp_pointer_constraints_v1_name");
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

static gpointer
spice_wayland_thread_run(gpointer data)
{
    GtkWidget *widget = GTK_WIDGET(data);
    struct wl_display *display = gdk_wayland_display_get_wl_display(gtk_widget_get_display(widget));
    struct wl_event_queue *queue = wl_display_create_queue(display);
    gint *control_fds = g_object_get_data(G_OBJECT(widget), "control_fds");
    gint control_read_fd = control_fds[0];
    GPollFD pollfd[2] = {
        { wl_display_get_fd(display), G_IO_IN, 0 },
        { control_read_fd, G_IO_HUP, 0 }
    };
    while (1) {
        while (wl_display_prepare_read_queue(display, queue) != 0) {
            wl_display_dispatch_queue_pending(display, queue);
        }
        wl_display_flush(display);
        if (g_poll(pollfd, 2, -1) == -1) {
            wl_display_cancel_read(display);
            goto error;
            break;
        }
        if (pollfd[1].revents & G_IO_HUP) {
            // The write end of the pipe is closed, exit the thread
            wl_display_cancel_read(display);
            break;
        }
        if (wl_display_read_events(display) == -1) {
            goto error;
            break;
        }
        wl_display_dispatch_queue_pending(display, queue);
    }
    return NULL;
error:
    g_warning("Failed to run event queue in spice-wayland-thread");
    return NULL;
}

void
spice_wayland_extensions_init(GtkWidget *widget)
{
    g_return_if_fail(GTK_IS_WIDGET(widget));

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display)) {
        return;
    }
    struct wl_display *display = gdk_wayland_display_get_wl_display(gdk_display);
    struct wl_event_queue *queue = wl_display_create_queue(display);
    struct wl_display *display_wrapper = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)display_wrapper, queue);
    struct wl_registry *registry = wl_display_get_registry(display_wrapper);
    wl_registry_add_listener(registry, &registry_listener, widget);

    wl_display_roundtrip_queue(display, queue);
    wl_display_roundtrip_queue(display, queue);

    g_object_set_data_full(G_OBJECT(widget),
                           "wl_display_wrapper",
                           display_wrapper,
                           (GDestroyNotify)wl_proxy_wrapper_destroy);
    g_object_set_data_full(G_OBJECT(widget),
                           "wl_event_queue",
                           queue,
                           (GDestroyNotify)wl_event_queue_destroy);
    g_object_set_data_full(G_OBJECT(widget),
                           "wl_registry",
                           registry,
                           (GDestroyNotify)wl_registry_destroy);

    // control_fds is used to communicate between the main thread and the created event queue thread
    // When the write end of the pipe is closed, the event queue thread will exit
    gint *control_fds = g_new(gint, 2);
    g_autoptr(GError) error = NULL;
    if (!g_unix_open_pipe(control_fds, O_CLOEXEC, &error)) {
        g_warning("Failed to create control pipe: %s", error->message);
        return;
    }
    g_object_set_data(G_OBJECT(widget), "control_fds", control_fds);

    GThread *thread = g_thread_new("spice-wayland-thread",
                                   spice_wayland_thread_run,
                                   widget);
    g_object_set_data(G_OBJECT(widget),
                      "spice-wayland-thread",
                      thread);
}

void
spice_wayland_extensions_finalize(GtkWidget *widget)
{
    g_return_if_fail(GTK_IS_WIDGET(widget));

    g_autofree gint *control_fds = g_object_get_data(G_OBJECT(widget), "control_fds");
    if (control_fds == NULL) {
        return;
    }
    gint control_write_fd = control_fds[1];
    close(control_write_fd);
    g_thread_join(g_object_get_data(G_OBJECT(widget), "spice-wayland-thread"));
}


static GdkDevice *
spice_gdk_window_get_pointing_device(GdkWindow *window)
{
    GdkDisplay *gdk_display = gdk_window_get_display(window);

    return gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display));
}

static struct zwp_relative_pointer_v1_listener relative_pointer_listener;

// NOTE this API works only on a single widget per application
int
spice_wayland_extensions_enable_relative_pointer(GtkWidget *widget,
                                                 void (*cb)(void *,
                                                            struct zwp_relative_pointer_v1 *,
                                                            uint32_t, uint32_t,
                                                            wl_fixed_t, wl_fixed_t, wl_fixed_t, wl_fixed_t))
{
    struct zwp_relative_pointer_v1 *relative_pointer;

    g_return_val_if_fail(GTK_IS_WIDGET(widget), -1);

    relative_pointer = g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_v1");

    if (relative_pointer == NULL) {
        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
        GdkWindow *window = gtk_widget_get_window(widget);
        struct wl_pointer *pointer;

        relative_pointer_manager = g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_manager_v1");
        if (relative_pointer_manager == NULL)
            return -1;

        pointer = gdk_wayland_device_get_wl_pointer(spice_gdk_window_get_pointing_device(window));
        relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_pointer_manager,
                                                                                pointer);

        relative_pointer_listener.relative_motion = cb;
        zwp_relative_pointer_v1_add_listener(relative_pointer,
                                             &relative_pointer_listener,
                                             widget);

        g_object_set_data_full(G_OBJECT(widget),
                               "zwp_relative_pointer_v1",
                               relative_pointer,
                               (GDestroyNotify)zwp_relative_pointer_v1_destroy);
    }

    return 0;
}

int spice_wayland_extensions_disable_relative_pointer(GtkWidget *widget)
{
    g_return_val_if_fail(GTK_IS_WIDGET(widget), -1);

    /* This will call zwp_relative_pointer_v1_destroy() and stop relative
     * movement */
    g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_v1", NULL);

    return 0;
}

static struct zwp_locked_pointer_v1_listener locked_pointer_listener;

// NOTE this API works only on a single widget per application
int
spice_wayland_extensions_lock_pointer(GtkWidget *widget,
                                      void (*lock_cb)(void *, struct zwp_locked_pointer_v1 *),
                                      void (*unlock_cb)(void *, struct zwp_locked_pointer_v1 *))
{
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_locked_pointer_v1 *locked_pointer;
    GdkWindow *window;
    struct wl_pointer *pointer;

    g_return_val_if_fail(GTK_IS_WIDGET(widget), -1);

    pointer_constraints = g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1");
    locked_pointer = g_object_get_data(G_OBJECT(widget), "zwp_locked_pointer_v1");
    if (locked_pointer != NULL) {
        /* A previous lock already in place */
        return 0;
    }

    window = gtk_widget_get_window(widget);
    pointer = gdk_wayland_device_get_wl_pointer(spice_gdk_window_get_pointing_device(window));
    locked_pointer = zwp_pointer_constraints_v1_lock_pointer(pointer_constraints,
                                                             gdk_wayland_window_get_wl_surface(window),
                                                             pointer,
                                                             NULL,
                                                             ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    if (lock_cb || unlock_cb) {
        locked_pointer_listener.locked = lock_cb;
        locked_pointer_listener.unlocked = unlock_cb;
        zwp_locked_pointer_v1_add_listener(locked_pointer,
                                           &locked_pointer_listener,
                                           widget);
    }
    g_object_set_data_full(G_OBJECT(widget),
                           "zwp_locked_pointer_v1",
                           locked_pointer,
                           (GDestroyNotify)zwp_locked_pointer_v1_destroy);

    return 0;
}

int
spice_wayland_extensions_unlock_pointer(GtkWidget *widget)
{
    g_return_val_if_fail(GTK_IS_WIDGET(widget), -1);

    g_object_set_data(G_OBJECT(widget), "zwp_locked_pointer_v1", NULL);

    return 0;
}
