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

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client-core.h>
#include <glib-unix.h>
#include "pointer-constraints-unstable-v1-client-protocol-gtk4.h"
#include "relative-pointer-unstable-v1-client-protocol-gtk4.h"

#include "wayland-extensions.h"

typedef void (*RelativeMotionCallback)(void *, struct zwp_relative_pointer_v1 *,
                                       uint32_t, uint32_t, wl_fixed_t, wl_fixed_t,
                                       wl_fixed_t, wl_fixed_t);
typedef void (*LockedPointerCallback)(void *, struct zwp_locked_pointer_v1 *);

typedef struct {
    GWeakRef widget;
    GThread *owner_thread;
    GMainContext *context;
    GMutex mutex;
    volatile gint ref_count;
    struct zwp_relative_pointer_v1 *relative;
    struct zwp_locked_pointer_v1 *locked;
    RelativeMotionCallback relative_cb;
    LockedPointerCallback lock_cb;
    LockedPointerCallback unlock_cb;
    guint relative_generation;
    guint locked_generation;
    GPtrArray *listener_contexts; /* Reclaimed after the worker has stopped. */
} SpiceWaylandState;
typedef struct {
    SpiceWaylandState *state;
    guint generation;
} SpiceWaylandListenerContext;

typedef struct {
    SpiceWaylandState *state;
    struct wl_registry *registry;
    uint32_t name;
    char *interface;
    gboolean removed;
} SpiceWaylandRegistryEvent;

static SpiceWaylandState *wayland_state_ref(SpiceWaylandState *state)
{
    g_atomic_int_inc(&state->ref_count);
    return state;
}

static void wayland_state_unref(SpiceWaylandState *state)
{
    if (!g_atomic_int_dec_and_test(&state->ref_count))
        return;
    g_ptr_array_unref(state->listener_contexts);
    g_weak_ref_clear(&state->widget);
    g_main_context_unref(state->context);
    g_mutex_clear(&state->mutex);
    g_free(state);
}

static void registry_global_on_main(GtkWidget *widget, struct wl_registry *registry,
                                    uint32_t name, const char *interface);
static void registry_remove_on_main(GtkWidget *widget, uint32_t name);

static void queue_on_main(SpiceWaylandState *state, GSourceFunc callback,
                          gpointer data, GDestroyNotify destroy)
{
    GSource *source = g_idle_source_new();
    g_source_set_callback(source, callback, data, destroy);
    g_source_attach(source, state->context);
    g_source_unref(source);
}

static void registry_event_free(gpointer data)
{
    SpiceWaylandRegistryEvent *event = data;
    g_free(event->interface);
    wayland_state_unref(event->state);
    g_free(event);
}

static gboolean registry_event_on_main(gpointer data)
{
    SpiceWaylandRegistryEvent *event = data;
    GtkWidget *widget = g_weak_ref_get(&event->state->widget);
    if (widget != NULL) {
        if (g_object_get_data(G_OBJECT(widget), "wayland-state") == event->state &&
            g_object_get_data(G_OBJECT(widget), "wl_registry") == event->registry) {
            if (event->removed)
                registry_remove_on_main(widget, event->name);
            else
                registry_global_on_main(widget, event->registry, event->name,
                                        event->interface);
        }
        g_object_unref(widget);
    }
    return G_SOURCE_REMOVE;
}

static void registry_event_queue(SpiceWaylandState *state,
                                 struct wl_registry *registry, uint32_t name,
                                 const char *interface, gboolean removed)
{
    SpiceWaylandRegistryEvent *event = g_new0(SpiceWaylandRegistryEvent, 1);
    event->state = wayland_state_ref(state);
    event->registry = registry;
    event->name = name;
    event->interface = g_strdup(interface);
    event->removed = removed;
    queue_on_main(state, registry_event_on_main, event, registry_event_free);
}

static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface,
                       uint32_t version G_GNUC_UNUSED)
{
    SpiceWaylandState *state = data;
    if (g_thread_self() == state->owner_thread) {
        GtkWidget *widget = g_weak_ref_get(&state->widget);
        if (widget != NULL) {
            registry_global_on_main(widget, registry, name, interface);
            g_object_unref(widget);
        }
    } else {
        registry_event_queue(state, registry, name, interface, FALSE);
    }
}

static void
registry_global_on_main(GtkWidget *widget, struct wl_registry *registry,
                        uint32_t name, const char *interface)
{
    if (g_strcmp0(interface, "zwp_relative_pointer_manager_v1") == 0) {
        struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
        relative_pointer_manager = wl_registry_bind(registry, name,
                                                    &zwp_relative_pointer_manager_v1_interface,
                                                    1);
        g_object_set_data_full(G_OBJECT(widget),
                               "zwp_relative_pointer_manager_v1",
                               relative_pointer_manager,
                               (GDestroyNotify)zwp_relative_pointer_manager_v1_destroy);
        g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name", GUINT_TO_POINTER(name));
    } else if (g_strcmp0(interface, "zwp_pointer_constraints_v1") == 0) {
        struct zwp_pointer_constraints_v1 *pointer_constraints;
        pointer_constraints = wl_registry_bind(registry, name,
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
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
    SpiceWaylandState *state = data;
    if (g_thread_self() == state->owner_thread) {
        GtkWidget *widget = g_weak_ref_get(&state->widget);
        if (widget != NULL) {
            registry_remove_on_main(widget, name);
            g_object_unref(widget);
        }
    } else {
        registry_event_queue(state, registry, name, NULL, TRUE);
    }
}

static void registry_remove_on_main(GtkWidget *widget, uint32_t name)
{
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    uint32_t relative_pointer_manager_name = 0;
    relative_pointer_manager = g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_manager_v1");
    relative_pointer_manager_name = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name"));
    if (relative_pointer_manager && relative_pointer_manager_name == name) {
        spice_wayland_extensions_disable_relative_pointer(widget);
        g_object_set_data_full(G_OBJECT(widget), "zwp_relative_pointer_manager_v1", NULL, NULL);
        g_object_steal_data(G_OBJECT(widget), "zwp_relative_pointer_v1_name");
    }

    struct zwp_pointer_constraints_v1 *pointer_constraints;
    uint32_t pointer_constraints_name = 0;
    pointer_constraints = g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1");
    pointer_constraints_name = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1_name"));
    if (pointer_constraints && pointer_constraints_name == name) {
        spice_wayland_extensions_unlock_pointer(widget);
        g_object_set_data_full(G_OBJECT(widget), "zwp_pointer_constraints_v1", NULL, NULL);
        g_object_steal_data(G_OBJECT(widget), "zwp_pointer_constraints_v1_name");
    }

}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove
};

typedef struct {
    struct wl_display *display;
    struct wl_event_queue *queue;
    gint read_fd;
} SpiceWaylandThreadData;

static gpointer
spice_wayland_thread_run(gpointer data)
{
    SpiceWaylandThreadData *thread_data = data;
    struct wl_display *display = thread_data->display;
    struct wl_event_queue *queue = thread_data->queue;
    GPollFD pollfd[2] = {
        { wl_display_get_fd(display), G_IO_IN, 0 },
        { thread_data->read_fd, G_IO_HUP, 0 }
    };
    while (1) {
        while (wl_display_prepare_read_queue(display, queue) != 0) {
            wl_display_dispatch_queue_pending(display, queue);
        }
        wl_display_flush(display);
        if (g_poll(pollfd, 2, -1) == -1) {
            wl_display_cancel_read(display);
            goto error;
        }
        if (pollfd[1].revents & G_IO_HUP) {
            // The write end of the pipe is closed, exit the thread
            wl_display_cancel_read(display);
            break;
        }
        if (wl_display_read_events(display) == -1)
            goto error;
        wl_display_dispatch_queue_pending(display, queue);
    }
    g_free(thread_data);
    return NULL;
error:
    g_warning("Failed to run event queue in spice-wayland-thread");
    g_free(thread_data);
    return NULL;
}

void
spice_wayland_extensions_init(GtkWidget *widget)
{
    g_return_if_fail(GTK_IS_WIDGET(widget));

    GdkDisplay *gdk_display = gtk_widget_get_display(widget);
    if (!GDK_IS_WAYLAND_DISPLAY(gdk_display) ||
        g_object_get_data(G_OBJECT(widget), "wl_registry"))
        return;

    gint *control_fds = g_new(gint, 2);
    GError *error = NULL;
    if (!g_unix_open_pipe(control_fds, O_CLOEXEC, &error)) {
        g_warning("Failed to create control pipe: %s", error->message);
        g_clear_error(&error);
        g_free(control_fds);
        return;
    }

    struct wl_display *display = gdk_wayland_display_get_wl_display(gdk_display);
    struct wl_event_queue *queue = wl_display_create_queue(display);
    struct wl_display *display_wrapper = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)display_wrapper, queue);
    struct wl_registry *registry = wl_display_get_registry(display_wrapper);
    SpiceWaylandState *state = g_new0(SpiceWaylandState, 1);
    g_weak_ref_init(&state->widget, widget);
    state->owner_thread = g_thread_self();
    state->context = g_main_context_ref_thread_default();
    g_mutex_init(&state->mutex);
    state->ref_count = 1;
    state->listener_contexts = g_ptr_array_new_with_free_func(g_free);
    g_object_set_data(G_OBJECT(widget), "wayland-state", state);
    wl_registry_add_listener(registry, &registry_listener, state);

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

    g_object_set_data(G_OBJECT(widget), "control_fds", control_fds);
    SpiceWaylandThreadData *thread_data = g_new(SpiceWaylandThreadData, 1);
    *thread_data = (SpiceWaylandThreadData) { display, queue, control_fds[0] };
    GThread *thread = g_thread_new("spice-wayland-thread",
                                   spice_wayland_thread_run, thread_data);
    g_object_set_data(G_OBJECT(widget), "spice-wayland-thread", thread);
}

void
spice_wayland_extensions_finalize(GtkWidget *widget)
{
    g_return_if_fail(GTK_IS_WIDGET(widget));
    SpiceWaylandState *state = g_object_get_data(G_OBJECT(widget), "wayland-state");
    if (state != NULL) {
        g_mutex_lock(&state->mutex);
        state->relative = NULL;
        state->locked = NULL;
        state->relative_generation++;
        state->locked_generation++;
        g_mutex_unlock(&state->mutex);
    }

    gint *control_fds = g_object_steal_data(G_OBJECT(widget), "control_fds");
    if (control_fds != NULL) {
        close(control_fds[1]);
        GThread *thread = g_object_steal_data(G_OBJECT(widget), "spice-wayland-thread");
        g_thread_join(thread);
        close(control_fds[0]);
        g_free(control_fds);
    }

    g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_v1", NULL);
    g_object_set_data(G_OBJECT(widget), "zwp_locked_pointer_v1", NULL);
    /* Listener functions are static; their state remains valid until the worker joins. */
    g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_manager_v1", NULL);
    g_object_set_data(G_OBJECT(widget), "zwp_pointer_constraints_v1", NULL);
    g_object_set_data(G_OBJECT(widget), "wl_registry", NULL);
    g_object_steal_data(G_OBJECT(widget), "wayland-state");
    g_object_set_data(G_OBJECT(widget), "wl_display_wrapper", NULL);
    g_object_set_data(G_OBJECT(widget), "wl_event_queue", NULL);
    if (state != NULL)
        wayland_state_unref(state);
}


static GdkDevice *
spice_gdk_window_get_pointing_device(GdkSurface *surface)
{
    GdkDisplay *gdk_display = gdk_surface_get_display(surface);

    GdkSeat *seat = gdk_display_get_default_seat(gdk_display);
    return seat ? gdk_seat_get_pointer(seat) : NULL;
}

typedef struct {
    SpiceWaylandState *state;
    struct zwp_relative_pointer_v1 *pointer;
    RelativeMotionCallback callback;
    guint generation;
    uint32_t time_hi, time_lo;
    wl_fixed_t dx, dy, dx_unaccel, dy_unaccel;
} SpiceRelativeMotionEvent;

static void relative_motion_event_free(gpointer data)
{
    SpiceRelativeMotionEvent *event = data;
    wayland_state_unref(event->state);
    g_free(event);
}

static gboolean relative_motion_on_main(gpointer data)
{
    SpiceRelativeMotionEvent *event = data;
    GtkWidget *widget = g_weak_ref_get(&event->state->widget);
    if (widget != NULL) {
        if (g_object_get_data(G_OBJECT(widget), "wayland-state") == event->state) {
            g_mutex_lock(&event->state->mutex);
            gboolean current = event->state->relative == event->pointer &&
                               event->state->relative_generation == event->generation;
            g_mutex_unlock(&event->state->mutex);
            if (current)
                event->callback(widget, event->pointer, event->time_hi,
                                event->time_lo, event->dx, event->dy,
                                event->dx_unaccel, event->dy_unaccel);
        }
        g_object_unref(widget);
    }
    return G_SOURCE_REMOVE;
}

static void relative_motion_received(void *data, struct zwp_relative_pointer_v1 *pointer,
                                     uint32_t time_hi, uint32_t time_lo,
                                     wl_fixed_t dx, wl_fixed_t dy,
                                     wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    SpiceWaylandListenerContext *listener_context = data;
    SpiceWaylandState *state = listener_context->state;
    g_mutex_lock(&state->mutex);
    RelativeMotionCallback callback = state->relative == pointer &&
                                      state->relative_generation == listener_context->generation
                                      ? state->relative_cb : NULL;
    guint generation = listener_context->generation;
    g_mutex_unlock(&state->mutex);
    if (callback == NULL)
        return;

    SpiceRelativeMotionEvent *event = g_new(SpiceRelativeMotionEvent, 1);
    *event = (SpiceRelativeMotionEvent) {
        wayland_state_ref(state), pointer, callback, generation, time_hi, time_lo,
        dx, dy, dx_unaccel, dy_unaccel
    };
    queue_on_main(state, relative_motion_on_main, event, relative_motion_event_free);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    relative_motion_received
};

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
        GtkNative *native = gtk_widget_get_native(widget);
        GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
        struct wl_pointer *pointer;

        relative_pointer_manager = g_object_get_data(G_OBJECT(widget), "zwp_relative_pointer_manager_v1");
        if (relative_pointer_manager == NULL || surface == NULL ||
            !GDK_IS_WAYLAND_SURFACE(surface))
            return -1;

        GdkDevice *device = spice_gdk_window_get_pointing_device(surface);
        if (device == NULL)
            return -1;
        pointer = gdk_wayland_device_get_wl_pointer(device);
        if (pointer == NULL)
            return -1;
        relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_pointer_manager,
                                                                                pointer);

        SpiceWaylandState *state = g_object_get_data(G_OBJECT(widget), "wayland-state");
        g_mutex_lock(&state->mutex);
        state->relative = relative_pointer;
        state->relative_cb = cb;
        state->relative_generation++;
        g_mutex_unlock(&state->mutex);
        SpiceWaylandListenerContext *listener_context = g_new(SpiceWaylandListenerContext, 1);
        *listener_context = (SpiceWaylandListenerContext) { state, state->relative_generation };
        g_ptr_array_add(state->listener_contexts, listener_context);
        zwp_relative_pointer_v1_add_listener(relative_pointer,
                                             &relative_pointer_listener, listener_context);

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

    SpiceWaylandState *state = g_object_get_data(G_OBJECT(widget), "wayland-state");
    if (state != NULL) {
        g_mutex_lock(&state->mutex);
        state->relative = NULL;
        state->relative_generation++;
        g_mutex_unlock(&state->mutex);
    }
    g_object_set_data(G_OBJECT(widget), "zwp_relative_pointer_v1", NULL);

    return 0;
}

typedef struct {
    SpiceWaylandState *state;
    struct zwp_locked_pointer_v1 *pointer;
    LockedPointerCallback callback;
    guint generation;
} SpiceLockedPointerEvent;

static void locked_pointer_event_free(gpointer data)
{
    SpiceLockedPointerEvent *event = data;
    wayland_state_unref(event->state);
    g_free(event);
}

static gboolean locked_pointer_on_main(gpointer data)
{
    SpiceLockedPointerEvent *event = data;
    GtkWidget *widget = g_weak_ref_get(&event->state->widget);
    if (widget != NULL) {
        if (g_object_get_data(G_OBJECT(widget), "wayland-state") == event->state) {
            g_mutex_lock(&event->state->mutex);
            gboolean current = event->state->locked == event->pointer &&
                               event->state->locked_generation == event->generation;
            g_mutex_unlock(&event->state->mutex);
            if (current)
                event->callback(widget, event->pointer);
        }
        g_object_unref(widget);
    }
    return G_SOURCE_REMOVE;
}

static void locked_pointer_received(void *data, struct zwp_locked_pointer_v1 *pointer,
                                    gboolean locked)
{
    SpiceWaylandListenerContext *listener_context = data;
    SpiceWaylandState *state = listener_context->state;
    g_mutex_lock(&state->mutex);
    LockedPointerCallback callback = state->locked == pointer &&
                                     state->locked_generation == listener_context->generation
                                     ? (locked ? state->lock_cb : state->unlock_cb) : NULL;
    g_mutex_unlock(&state->mutex);
    if (callback == NULL)
        return;

    SpiceLockedPointerEvent *event = g_new(SpiceLockedPointerEvent, 1);
    *event = (SpiceLockedPointerEvent) {
        wayland_state_ref(state), pointer, callback, listener_context->generation
    };
    queue_on_main(state, locked_pointer_on_main, event, locked_pointer_event_free);
}

static void locked_pointer_locked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    locked_pointer_received(data, pointer, TRUE);
}

static void locked_pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *pointer)
{
    locked_pointer_received(data, pointer, FALSE);
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    locked_pointer_locked, locked_pointer_unlocked
};

int
spice_wayland_extensions_lock_pointer(GtkWidget *widget,
                                      void (*lock_cb)(void *, struct zwp_locked_pointer_v1 *),
                                      void (*unlock_cb)(void *, struct zwp_locked_pointer_v1 *))
{
    struct zwp_pointer_constraints_v1 *pointer_constraints;
    struct zwp_locked_pointer_v1 *locked_pointer;
    GdkSurface *surface;
    struct wl_pointer *pointer;

    g_return_val_if_fail(GTK_IS_WIDGET(widget), -1);

    pointer_constraints = g_object_get_data(G_OBJECT(widget), "zwp_pointer_constraints_v1");
    locked_pointer = g_object_get_data(G_OBJECT(widget), "zwp_locked_pointer_v1");
    if (locked_pointer != NULL) {
        /* A previous lock already in place */
        return 0;
    }

    GtkNative *native = gtk_widget_get_native(widget);
    surface = native ? gtk_native_get_surface(native) : NULL;
    if (pointer_constraints == NULL || surface == NULL ||
        !GDK_IS_WAYLAND_SURFACE(surface))
        return -1;
    GdkDevice *device = spice_gdk_window_get_pointing_device(surface);
    if (device == NULL)
        return -1;
    pointer = gdk_wayland_device_get_wl_pointer(device);
    if (pointer == NULL)
        return -1;
    locked_pointer = zwp_pointer_constraints_v1_lock_pointer(pointer_constraints,
                                                             gdk_wayland_surface_get_wl_surface(surface),
                                                             pointer,
                                                             NULL,
                                                             ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    SpiceWaylandState *state = g_object_get_data(G_OBJECT(widget), "wayland-state");
    g_mutex_lock(&state->mutex);
    state->locked = locked_pointer;
    state->lock_cb = lock_cb;
    state->unlock_cb = unlock_cb;
    state->locked_generation++;
    g_mutex_unlock(&state->mutex);
    if (lock_cb || unlock_cb) {
        SpiceWaylandListenerContext *listener_context = g_new(SpiceWaylandListenerContext, 1);
        *listener_context = (SpiceWaylandListenerContext) { state, state->locked_generation };
        g_ptr_array_add(state->listener_contexts, listener_context);
        zwp_locked_pointer_v1_add_listener(locked_pointer,
                                           &locked_pointer_listener, listener_context);
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

    SpiceWaylandState *state = g_object_get_data(G_OBJECT(widget), "wayland-state");
    if (state != NULL) {
        g_mutex_lock(&state->mutex);
        state->locked = NULL;
        state->locked_generation++;
        g_mutex_unlock(&state->mutex);
    }
    g_object_set_data(G_OBJECT(widget), "zwp_locked_pointer_v1", NULL);

    return 0;
}

