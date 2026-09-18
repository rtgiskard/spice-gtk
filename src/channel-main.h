/*
   Copyright (C) 2010 Red Hat, Inc.

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

#include "spice-client.h"

G_BEGIN_DECLS

#define SPICE_TYPE_MAIN_CHANNEL            (spice_main_channel_get_type())
#define SPICE_MAIN_CHANNEL(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), SPICE_TYPE_MAIN_CHANNEL, SpiceMainChannel))
#define SPICE_MAIN_CHANNEL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), SPICE_TYPE_MAIN_CHANNEL, SpiceMainChannelClass))
#define SPICE_IS_MAIN_CHANNEL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), SPICE_TYPE_MAIN_CHANNEL))
#define SPICE_IS_MAIN_CHANNEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), SPICE_TYPE_MAIN_CHANNEL))
#define SPICE_MAIN_CHANNEL_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), SPICE_TYPE_MAIN_CHANNEL, SpiceMainChannelClass))

typedef struct _SpiceMainChannel SpiceMainChannel;
typedef struct _SpiceMainChannelClass SpiceMainChannelClass;
typedef struct _SpiceMainChannelPrivate SpiceMainChannelPrivate;

/**
 * SpiceMainChannel:
 *
 * The #SpiceMainChannel struct is opaque and should not be accessed directly.
 */
struct _SpiceMainChannel {
    SpiceChannel parent;

    /*< private >*/
    SpiceMainChannelPrivate *priv;
    /* Do not add fields to this struct */
};

/**
 * SpiceMainChannelClass:
 * @parent_class: Parent class.
 * @mouse_update: Signal class handler for the #SpiceMainChannel::main-mouse-update signal.
 * @agent_update: Signal class handler for the #SpiceMainChannel::main-agent-update signal.
 *
 * Class structure for #SpiceMainChannel.
 */
struct _SpiceMainChannelClass {
    SpiceChannelClass parent_class;

    /* signals */
    void (*mouse_update)(SpiceChannel *channel);
    void (*agent_update)(SpiceChannel *channel);

    /*< private >*/
    /* Do not add fields to this struct */
};

SPICE_GTK_AVAILABLE_IN_ALL
GType spice_main_channel_get_type(void);

SPICE_GTK_AVAILABLE_IN_0_39
void spice_main_channel_update_display_mm(SpiceMainChannel *channel, int id,
                                          int width_mm, int height_mm, gboolean update);

SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_update_display(SpiceMainChannel *channel, int id, int x, int y, int width,
                                       int height, gboolean update);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_update_display_enabled(SpiceMainChannel *channel, int id, gboolean enabled,
                                               gboolean update);
SPICE_GTK_AVAILABLE_IN_0_35
gboolean spice_main_channel_send_monitor_config(SpiceMainChannel *channel);

SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_clipboard_selection_grab(SpiceMainChannel *channel, guint selection,
                                                 guint32 *types, int ntypes);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_clipboard_selection_release(SpiceMainChannel *channel, guint selection);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_clipboard_selection_notify(SpiceMainChannel *channel, guint selection,
                                                   guint32 type, const guchar *data, size_t size);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_clipboard_selection_request(SpiceMainChannel *channel, guint selection,
                                                    guint32 type);

SPICE_GTK_AVAILABLE_IN_0_35
gboolean spice_main_channel_agent_test_capability(SpiceMainChannel *channel, guint32 cap);
SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_file_copy_async(SpiceMainChannel *channel,
                                        GFile **sources,
                                        GFileCopyFlags flags,
                                        GCancellable *cancellable,
                                        GFileProgressCallback progress_callback,
                                        gpointer progress_callback_data,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

SPICE_GTK_AVAILABLE_IN_0_35
gboolean spice_main_channel_file_copy_finish(SpiceMainChannel *channel,
                                             GAsyncResult *result,
                                             GError **error);

SPICE_GTK_AVAILABLE_IN_0_35
void spice_main_channel_request_mouse_mode(SpiceMainChannel *channel, int mode);

#ifndef SPICE_DISABLE_DEPRECATED
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_grab)
void spice_main_clipboard_grab(SpiceMainChannel *channel, guint32 *types, int ntypes);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_release)
void spice_main_clipboard_release(SpiceMainChannel *channel);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_notify)
void spice_main_clipboard_notify(SpiceMainChannel *channel, guint32 type, const guchar *data, size_t size);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_request)
void spice_main_clipboard_request(SpiceMainChannel *channel, guint32 type);

SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_update_display)
void spice_main_set_display(SpiceMainChannel *channel, int id,int x, int y, int width, int height);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_update_display)
void spice_main_update_display(SpiceMainChannel *channel, int id, int x, int y, int width,
                               int height, gboolean update);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_update_display_enabled)
void spice_main_set_display_enabled(SpiceMainChannel *channel, int id, gboolean enabled);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_update_display_enabled)
void spice_main_update_display_enabled(SpiceMainChannel *channel, int id, gboolean enabled,
                                       gboolean update);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_send_monitor_config)
gboolean spice_main_send_monitor_config(SpiceMainChannel *channel);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_grab)
void spice_main_clipboard_selection_grab(SpiceMainChannel *channel, guint selection, guint32 *types,
                                         int ntypes);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_release)
void spice_main_clipboard_selection_release(SpiceMainChannel *channel, guint selection);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_notify)
void spice_main_clipboard_selection_notify(SpiceMainChannel *channel, guint selection, guint32 type,
                                           const guchar *data, size_t size);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_clipboard_selection_request)
void spice_main_clipboard_selection_request(SpiceMainChannel *channel, guint selection,
                                            guint32 type);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_agent_test_capability)
gboolean spice_main_agent_test_capability(SpiceMainChannel *channel, guint32 cap);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_file_copy_async)
void spice_main_file_copy_async(SpiceMainChannel *channel, GFile **sources, GFileCopyFlags flags,
                                GCancellable *cancellable, GFileProgressCallback progress_callback,
                                gpointer progress_callback_data, GAsyncReadyCallback callback,
                                gpointer user_data);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_file_copy_finish)
gboolean spice_main_file_copy_finish(SpiceMainChannel *channel, GAsyncResult *result,
                                     GError **error);
SPICE_GTK_DEPRECATED_IN_0_35_FOR(spice_main_channel_request_mouse_mode)
void spice_main_request_mouse_mode(SpiceMainChannel *channel, int mode);
#endif

G_DEFINE_AUTOPTR_CLEANUP_FUNC (SpiceMainChannel, g_object_unref)
G_END_DECLS
