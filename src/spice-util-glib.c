/*
   Copyright (C) 2010 Red Hat, Inc.
   Copyright © 2006-2010 Collabora Ltd. <http://www.collabora.co.uk/>

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
#include "config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include "spice-common.h"
#include "spice-util-priv.h"
#include "spice-util.h"
#include "spice-util-priv.h"

static GOnce debug_once = G_ONCE_INIT;

static void spice_util_enable_debug_messages(void)
{
    const gchar *doms = g_getenv("G_MESSAGES_DEBUG");
    if (!doms) {
        g_setenv("G_MESSAGES_DEBUG", G_LOG_DOMAIN, 1);
    } else if (g_str_equal(doms, "all")) {
        return;
    } else if (!strstr(doms, G_LOG_DOMAIN)) {
        g_autofree gchar *newdoms = g_strdup_printf("%s %s", doms, G_LOG_DOMAIN);
        g_setenv("G_MESSAGES_DEBUG", newdoms, 1);
    }
}

/**
 * spice_glib_set_debug:
 * @enabled: %TRUE or %FALSE
 *
 * Enable or disable Spice-GTK debugging messages.
 * Since: 0.43
 **/
void spice_glib_set_debug(gboolean enabled)
{
    /* Make sure debug_once has been initialised
     * with the value of SPICE_DEBUG already, otherwise
     * spice_util_get_debug() may overwrite the value
     * that was just set using spice_util_set_debug()
     */
    spice_glib_get_debug();

    if (enabled) {
        spice_util_enable_debug_messages();
    }

    debug_once.retval = GINT_TO_POINTER(enabled);
}

static gpointer getenv_debug(gpointer data)
{
    gboolean debug;

    debug = (g_getenv("SPICE_DEBUG") != NULL);
    if (debug)
        spice_util_enable_debug_messages();

    return GINT_TO_POINTER(debug);
}

gboolean spice_glib_get_debug(void)
{
    g_once(&debug_once, getenv_debug, NULL);

    return GPOINTER_TO_INT(debug_once.retval);
}

/**
 * spice_glib_get_version_string:
 *
 * Gets the version string
 *
 * Returns: Spice-GTK version as a const string.
 * Since: 0.43
 **/
const gchar *spice_glib_get_version_string(void)
{
    return VERSION;
}

/**
 * spice_glib_uuid_to_string:
 * @uuid: (array fixed-size=16): UUID byte array
 *
 * Creates a string representation of @uuid, of the form
 * "06e023d5-86d8-420e-8103-383e4566087a"
 *
 * Returns: A string that should be freed with g_free().
 * Since: 0.43
 **/
gchar* spice_glib_uuid_to_string(const guint8 uuid[16])
{
    return g_strdup_printf(UUID_FMT, uuid[0], uuid[1],
                           uuid[2], uuid[3], uuid[4], uuid[5],
                           uuid[6], uuid[7], uuid[8], uuid[9],
                           uuid[10], uuid[11], uuid[12], uuid[13],
                           uuid[14], uuid[15]);
}

typedef struct {
    GObject *instance;
    GObject *observer;
    GClosure *closure;
    gulong handler_id;
} WeakHandlerCtx;

static WeakHandlerCtx *
whc_new (GObject *instance,
         GObject *observer)
{
    WeakHandlerCtx *ctx = g_new0 (WeakHandlerCtx, 1);

    ctx->instance = instance;
    ctx->observer = observer;

    return ctx;
}

static void
whc_free (WeakHandlerCtx *ctx)
{
    g_free (ctx);
}

static void observer_destroyed_cb (gpointer, GObject *);
static void closure_invalidated_cb (gpointer, GClosure *);

/*
 * If signal handlers are removed before the object is destroyed, this
 * callback will never get triggered.
 */
static void
instance_destroyed_cb (gpointer ctx_,
                       GObject *where_the_instance_was)
{
    WeakHandlerCtx *ctx = ctx_;

    /* No need to disconnect the signal here, the instance has gone away. */
    g_object_weak_unref (ctx->observer, observer_destroyed_cb, ctx);
    g_closure_remove_invalidate_notifier (ctx->closure, ctx,
                                          closure_invalidated_cb);
    whc_free (ctx);
}

/* Triggered when the observer is destroyed. */
static void
observer_destroyed_cb (gpointer ctx_,
                       GObject *where_the_observer_was)
{
    WeakHandlerCtx *ctx = ctx_;

    g_closure_remove_invalidate_notifier (ctx->closure, ctx,
                                          closure_invalidated_cb);
    g_signal_handler_disconnect (ctx->instance, ctx->handler_id);
    g_object_weak_unref (ctx->instance, instance_destroyed_cb, ctx);
    whc_free (ctx);
}

/* Triggered when either object is destroyed or the handler is disconnected. */
static void
closure_invalidated_cb (gpointer ctx_,
                        GClosure *where_the_closure_was)
{
    WeakHandlerCtx *ctx = ctx_;

    g_object_weak_unref (ctx->instance, instance_destroyed_cb, ctx);
    g_object_weak_unref (ctx->observer, observer_destroyed_cb, ctx);
    whc_free (ctx);
}

/* Copied from tp_g_signal_connect_object. See documentation. */
/**
 * spice_glib_signal_connect_object: (skip)
 * @instance: the instance to connect to.
 * @detailed_signal: a string of the form "signal-name::detail".
 * @c_handler: the #GCallback to connect.
 * @gobject: the object to pass as data to @c_handler.
 * @connect_flags: a combination of #GConnectFlags.
 *
 * Similar to g_signal_connect_object() but will delete connection
 * when any of the objects is destroyed.
 *
 * Returns: the handler id.
 * Since: 0.43
 */
gulong spice_glib_signal_connect_object (gpointer instance,
                                         const gchar *detailed_signal,
                                         GCallback c_handler,
                                         gpointer gobject,
                                         GConnectFlags connect_flags)
{
    GObject *instance_obj = G_OBJECT (instance);

    g_return_val_if_fail (G_TYPE_CHECK_INSTANCE (instance), 0);
    g_return_val_if_fail (detailed_signal != NULL, 0);
    g_return_val_if_fail (c_handler != NULL, 0);
    g_return_val_if_fail (G_IS_OBJECT (gobject), 0);
    g_return_val_if_fail (
                          (connect_flags & ~(G_CONNECT_AFTER|G_CONNECT_SWAPPED)) == 0, 0);

    WeakHandlerCtx *ctx = whc_new (instance_obj, gobject);
    if (connect_flags & G_CONNECT_SWAPPED)
        ctx->closure = g_cclosure_new_object_swap (c_handler, gobject);
    else
        ctx->closure = g_cclosure_new_object (c_handler, gobject);

    ctx->handler_id = g_signal_connect_closure (instance, detailed_signal,
                                                ctx->closure, (connect_flags & G_CONNECT_AFTER) ? TRUE : FALSE);

    g_object_weak_ref (instance_obj, instance_destroyed_cb, ctx);
    g_object_weak_ref (gobject, observer_destroyed_cb, ctx);
    g_closure_add_invalidate_notifier (ctx->closure, ctx,
                                       closure_invalidated_cb);

    return ctx->handler_id;
}
