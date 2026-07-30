/*
   Copyright (C) 2012 Red Hat, Inc.

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
#pragma once

#if !defined(__SPICE_CLIENT_H_INSIDE__) && !defined(SPICE_COMPILATION)
#warning "Only <spice-client.h> can be included directly"
#endif

#include <gio/gio.h>
#include "spice-channel.h"

G_BEGIN_DECLS

#define SPICE_TYPE_PORT_CHANNEL            (spice_port_channel_get_type())
#define SPICE_PORT_CHANNEL(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), SPICE_TYPE_PORT_CHANNEL, SpicePortChannel))
#define SPICE_PORT_CHANNEL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), SPICE_TYPE_PORT_CHANNEL, SpicePortChannelClass))
#define SPICE_IS_PORT_CHANNEL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), SPICE_TYPE_PORT_CHANNEL))
#define SPICE_IS_PORT_CHANNEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), SPICE_TYPE_PORT_CHANNEL))
#define SPICE_PORT_CHANNEL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), SPICE_TYPE_PORT_CHANNEL, SpicePortChannelClass))

typedef struct _SpicePortChannel SpicePortChannel;
typedef struct _SpicePortChannelClass SpicePortChannelClass;
typedef struct _SpicePortChannelPrivate SpicePortChannelPrivate;

/**
 * SpicePortChannel:
 *
 * The #SpicePortChannel struct is opaque and should not be accessed directly.
 */
struct _SpicePortChannel {
    SpiceChannel parent;

    /*< private >*/
    SpicePortChannelPrivate *priv;
    /* Do not add fields to this struct */
};

/**
 * SpicePortChannelClass:
 * @parent_class: Parent class.
 *
 * Class structure for #SpicePortChannel.
 */
struct _SpicePortChannelClass {
    SpiceChannelClass parent_class;

    /*< private >*/
    /* Do not add fields to this struct */
};

SPICE_GTK_AVAILABLE_IN_0_15
GType spice_port_channel_get_type(void);

SPICE_GTK_AVAILABLE_IN_0_35
void spice_port_channel_write_async(SpicePortChannel *port,
                                    const void *buffer, gsize count,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);
SPICE_GTK_AVAILABLE_IN_0_35
gssize spice_port_channel_write_finish(SpicePortChannel *port,
                                       GAsyncResult *result, GError **error);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_port_channel_event(SpicePortChannel *port, guint8 event);


#ifndef SPICE_DISABLE_DEPRECATED
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_port_channel_write_async)
void spice_port_write_async(SpicePortChannel *port,
                            const void *buffer, gsize count,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_port_channel_write_finish)
gssize spice_port_write_finish(SpicePortChannel *port,
                               GAsyncResult *result, GError **error);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_port_channel_event)
void spice_port_event(SpicePortChannel *port, guint8 event);
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpicePortChannel, g_object_unref)
G_END_DECLS
