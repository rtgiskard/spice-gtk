/*
   Copyright (C) 2010-2011 Red Hat, Inc.

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

#include <glib.h>
#include <gdk/gdk.h>

#ifdef HAVE_X11_XKBLIB_H
#include <X11/XKBlib.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#endif
#ifdef G_OS_WIN32
#include <windows.h>
#include <gdk/win32/gdkwin32.h>
#ifndef MAPVK_VK_TO_VSC /* may be undefined in older mingw-headers */
#define MAPVK_VK_TO_VSC 0
#endif
#endif

#ifdef HAVE_PHODAV_VIRTUAL
#include <libphodav/phodav.h>
#endif

#include <gtk/gtk.h>
#include <spice/vd_agent.h>
#include "desktop-integration.h"
#include "spice-common.h"
#include "spice-gtk-session.h"
#include "spice-gtk-session-priv.h"
#include "spice-session-priv.h"
#include "spice-util-priv.h"
#include "spice-channel-priv.h"

#define CLIPBOARD_LAST (VD_AGENT_CLIPBOARD_SELECTION_SECONDARY + 1)

struct _SpiceGtkSessionPrivate {
    SpiceSession            *session;
    /* Clipboard related */
    gboolean                auto_clipboard_enable;
    SpiceMainChannel        *main;
    GdkClipboard            *clipboard;
    GdkClipboard            *clipboard_primary;
    /* GWeakRef session identities are cleared before dispose; retain weak
     * provider identities so disposal can clear only our installed content. */
    GWeakRef                guest_provider[CLIPBOARD_LAST];
    /* Guest targets are indices into the immutable atom2agent table. */
    guint                   nclip_targets[CLIPBOARD_LAST];
    const char              **atoms[CLIPBOARD_LAST];
    guint                   n_atoms[CLIPBOARD_LAST];
    guint                   *clip_target_info[CLIPBOARD_LAST];
    gboolean                clip_hasdata[CLIPBOARD_LAST];
    gboolean                clip_grabbed[CLIPBOARD_LAST];
    gboolean                clipboard_by_guest[CLIPBOARD_LAST];
    guint                   clipboard_release_delay[CLIPBOARD_LAST];
    GList                   *pending_reads; /* async host-to-guest reads */
    guint                   clipboard_generation[CLIPBOARD_LAST];
    /* TODO: maybe add a way of restoring this? */
    GHashTable              *cb_shared_files;
    /* auto-usbredir related */
    gboolean                auto_usbredir_enable;
    int                     auto_usbredir_reqs;
    gboolean                pointer_grabbed;
    gboolean                keyboard_has_focus;
    gboolean                mouse_has_pointer;
    gboolean                sync_modifiers;
};

/**
 * SECTION:spice-gtk-session
 * @short_description: handles GTK connection details
 * @title: Spice GTK Session
 * @section_id:
 * @see_also: #SpiceSession, and the GTK widget #SpiceDisplay
 * @stability: Stable
 * @include: spice-client-gtk.h
 *
 * The #SpiceGtkSession class is the spice-client-gtk counter part of
 * #SpiceSession. It contains functionality which should be handled per
 * session rather then per #SpiceDisplay (one session can have multiple
 * displays), but which cannot live in #SpiceSession as it depends on
 * GTK. For example the clipboard functionality.
 *
 * There should always be a 1:1 relation between #SpiceGtkSession objects
 * and #SpiceSession objects. Therefor there is no spice_gtk_session_new,
 * instead there is spice_gtk_session_get() which ensures this 1:1 relation.
 *
 * Client and guest clipboards will be shared automatically if
 * #SpiceGtkSession:auto-clipboard is set to #TRUE. Alternatively, you
 * can send / receive clipboard data from client to guest with
 * spice_gtk_session_copy_to_guest() / spice_gtk_session_paste_from_guest().
 */

/* ------------------------------------------------------------------ */
/* Prototypes for private functions */
static void clipboard_release(SpiceGtkSession *self, guint selection);
static void clipboard_release_delay_remove(SpiceGtkSession *self, guint selection,
                                           gboolean release_if_delayed);
static void clipboard_owner_change(GdkClipboard *clipboard,
                                   gpointer user_data);
static void clipboard_cancel_reads(SpiceGtkSession *self, int selection);
static gboolean clipboard_is_owned(SpiceGtkSession *self, GdkClipboard *clipboard,
                                    guint selection);
static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data);
static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data);
static gboolean read_only(SpiceGtkSession *self);

G_DEFINE_TYPE_WITH_PRIVATE(SpiceGtkSession, spice_gtk_session, G_TYPE_OBJECT)

/* Properties */
enum {
    PROP_0,
    PROP_SESSION,
    PROP_AUTO_CLIPBOARD,
    PROP_AUTO_USBREDIR,
    PROP_POINTER_GRABBED,
    PROP_SYNC_MODIFIERS,
};

static guint32 get_keyboard_lock_modifiers(void)
{
    guint32 modifiers = 0;
    GdkSeat *seat = gdk_display_get_default_seat(gdk_display_get_default());
    GdkDevice *keyboard = gdk_seat_get_keyboard(seat);

    if (gdk_device_get_caps_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_CAPS_LOCK;
    }

    if (gdk_device_get_num_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_NUM_LOCK;
    }

    if (gdk_device_get_scroll_lock_state(keyboard)) {
        modifiers |= SPICE_INPUTS_SCROLL_LOCK;
    }
    return modifiers;
}

static void spice_gtk_session_sync_keyboard_modifiers_for_channel(SpiceGtkSession *self,
                                                                  SpiceInputsChannel* inputs,
                                                                  gboolean force)
{
    guint32 guest_modifiers = 0, client_modifiers = 0;

    g_return_if_fail(SPICE_IS_INPUTS_CHANNEL(inputs));

    if (SPICE_IS_GTK_SESSION(self) && !self->priv->sync_modifiers) {
        SPICE_DEBUG("Syncing modifiers is disabled");
        return;
    }

    g_object_get(inputs, "key-modifiers", &guest_modifiers, NULL);
    client_modifiers = get_keyboard_lock_modifiers();

    if (force || client_modifiers != guest_modifiers) {
        CHANNEL_DEBUG(inputs, "client_modifiers:0x%x, guest_modifiers:0x%x",
                      client_modifiers, guest_modifiers);
        spice_inputs_channel_set_key_locks(inputs, client_modifiers);
    }
}

static void keymap_modifiers_changed(gpointer source G_GNUC_UNUSED, gpointer data)
{
    SpiceGtkSession *self = data;

    /* set_key_locks() is inherently racy, but no need to sync modifiers
     * if we have focus as the regular keypress/keyrelease will have set
     * the expected modifiers state in the guest.
     */
    if (self->priv->keyboard_has_focus) {
        return;
    }

    spice_gtk_session_sync_keyboard_modifiers(self);
}

static void guest_modifiers_changed(SpiceInputsChannel *inputs, gpointer data)
{
    SpiceGtkSession *self = data;

    spice_gtk_session_sync_keyboard_modifiers_for_channel(self, inputs, FALSE);
}

static void spice_gtk_session_init(SpiceGtkSession *self)
{
    SpiceGtkSessionPrivate *s;
    GdkSeat *seat;

    s = self->priv = spice_gtk_session_get_instance_private(self);
    for (guint i = 0; i < CLIPBOARD_LAST; i++)
        g_weak_ref_init(&s->guest_provider[i], NULL);

    s->cb_shared_files =
        g_hash_table_new_full(g_file_hash,
                              (GEqualFunc)g_file_equal,
                              g_object_unref, /* unref GFile */
                              g_free /* free gchar * */
                             );
    GdkDisplay *display = gdk_display_get_default();
    s->clipboard = gdk_display_get_clipboard(display);
    g_signal_connect(s->clipboard, "changed",
                     G_CALLBACK(clipboard_owner_change), self);
    s->clipboard_primary = gdk_display_get_primary_clipboard(display);
    g_signal_connect(s->clipboard_primary, "changed",
                     G_CALLBACK(clipboard_owner_change), self);

    seat = gdk_display_get_default_seat(display);
    spice_g_signal_connect_object(gdk_seat_get_keyboard(seat),
                                  "changed",
                                  G_CALLBACK(keymap_modifiers_changed), self, 0);
}

static void
spice_gtk_session_constructed(GObject *gobject)
{
    SpiceGtkSession *self;
    SpiceGtkSessionPrivate *s;
    GList *list;
    GList *it;

    self = SPICE_GTK_SESSION(gobject);
    s = self->priv;
    if (!s->session)
        g_error("SpiceGtKSession constructed without a session");

    g_signal_connect(s->session, "channel-new",
                     G_CALLBACK(channel_new), self);
    g_signal_connect(s->session, "channel-destroy",
                     G_CALLBACK(channel_destroy), self);
    list = spice_session_get_channels(s->session);
    for (it = g_list_first(list); it != NULL; it = g_list_next(it)) {
        channel_new(s->session, it->data, (gpointer*)self);
    }
    g_list_free(list);
}

static void spice_gtk_session_dispose(GObject *gobject)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;
    clipboard_cancel_reads(self, -1);
    for (guint i = 0; i < CLIPBOARD_LAST; i++)
        clipboard_release_delay_remove(self, i, FALSE);
    if (s->main) {
        g_signal_handlers_disconnect_by_data(s->main, self);
        s->main = NULL;
    }
    if (s->clipboard && clipboard_is_owned(self, s->clipboard,
                                           VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD))
        gdk_clipboard_set_content(s->clipboard, NULL);
    if (s->clipboard_primary && clipboard_is_owned(self, s->clipboard_primary,
                                                   VD_AGENT_CLIPBOARD_SELECTION_PRIMARY))
        gdk_clipboard_set_content(s->clipboard_primary, NULL);


    /* release stuff */
    if (s->clipboard) {
        g_signal_handlers_disconnect_by_func(s->clipboard,
                G_CALLBACK(clipboard_owner_change), self);
        s->clipboard = NULL;
    }

    if (s->clipboard_primary) {
        g_signal_handlers_disconnect_by_func(s->clipboard_primary,
                G_CALLBACK(clipboard_owner_change), self);
        s->clipboard_primary = NULL;
    }

    if (s->session) {
        g_signal_handlers_disconnect_by_func(s->session,
                                             G_CALLBACK(channel_new),
                                             self);
        g_signal_handlers_disconnect_by_func(s->session,
                                             G_CALLBACK(channel_destroy),
                                             self);
        s->session = NULL;
    }
    g_clear_pointer(&s->cb_shared_files, g_hash_table_destroy);

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_gtk_session_parent_class)->dispose)
        G_OBJECT_CLASS(spice_gtk_session_parent_class)->dispose(gobject);
}

static void clipboard_release_delay_remove(SpiceGtkSession *self, guint selection,
                                           gboolean release_if_delayed)
{
    SpiceGtkSessionPrivate *s = self->priv;

    if (!s->clipboard_release_delay[selection]) {
        return;
    }

    if (release_if_delayed) {
        SPICE_DEBUG("delayed clipboard release, sel:%u", selection);
        clipboard_release(self, selection);
    }

    g_source_remove(s->clipboard_release_delay[selection]);
    s->clipboard_release_delay[selection] = 0;
}

static void spice_gtk_session_finalize(GObject *gobject)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;
    int i;

    /* release stuff */
    for (i = 0; i < CLIPBOARD_LAST; ++i) {
        g_weak_ref_clear(&s->guest_provider[i]);
        g_clear_pointer(&s->clip_target_info[i], g_free);
        clipboard_release_delay_remove(self, i, true);
        g_clear_pointer(&s->atoms[i], g_free);
        s->n_atoms[i] = 0;
    }

    /* Chain up to the parent class */
    if (G_OBJECT_CLASS(spice_gtk_session_parent_class)->finalize)
        G_OBJECT_CLASS(spice_gtk_session_parent_class)->finalize(gobject);
}

static void spice_gtk_session_get_property(GObject    *gobject,
                                           guint       prop_id,
                                           GValue     *value,
                                           GParamSpec *pspec)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, s->session);
	break;
    case PROP_AUTO_CLIPBOARD:
        g_value_set_boolean(value, s->auto_clipboard_enable);
        break;
    case PROP_AUTO_USBREDIR:
        g_value_set_boolean(value, s->auto_usbredir_enable);
        break;
    case PROP_POINTER_GRABBED:
        g_value_set_boolean(value, s->pointer_grabbed);
        break;
    case PROP_SYNC_MODIFIERS:
        g_value_set_boolean(value, s->sync_modifiers);
        break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
	break;
    }
}

static void spice_gtk_session_set_property(GObject      *gobject,
                                           guint         prop_id,
                                           const GValue *value,
                                           GParamSpec   *pspec)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(gobject);
    SpiceGtkSessionPrivate *s = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        s->session = g_value_get_object(value);
        break;
    case PROP_AUTO_CLIPBOARD:
        s->auto_clipboard_enable = g_value_get_boolean(value);
        break;
    case PROP_AUTO_USBREDIR: {
        SpiceDesktopIntegration *desktop_int;
        gboolean orig_value = s->auto_usbredir_enable;

        s->auto_usbredir_enable = g_value_get_boolean(value);
        if (s->auto_usbredir_enable == orig_value)
            break;

        if (s->auto_usbredir_reqs) {
            SpiceUsbDeviceManager *manager =
                spice_usb_device_manager_get(s->session, NULL);

            if (!manager)
                break;

            g_object_set(manager, "auto-connect", s->auto_usbredir_enable,
                         NULL);

            desktop_int = spice_desktop_integration_get(s->session);
            if (s->auto_usbredir_enable)
                spice_desktop_integration_inhibit_automount(desktop_int);
            else
                spice_desktop_integration_uninhibit_automount(desktop_int);
        }
        break;
    }
    case PROP_SYNC_MODIFIERS:
        s->sync_modifiers = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_gtk_session_class_init(SpiceGtkSessionClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

    gobject_class->constructed  = spice_gtk_session_constructed;
    gobject_class->dispose      = spice_gtk_session_dispose;
    gobject_class->finalize     = spice_gtk_session_finalize;
    gobject_class->get_property = spice_gtk_session_get_property;
    gobject_class->set_property = spice_gtk_session_set_property;

    /**
     * SpiceGtkSession:session:
     *
     * #SpiceSession this #SpiceGtkSession is associated with
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_SESSION,
         g_param_spec_object("session",
                             "Session",
                             "SpiceSession",
                             SPICE_TYPE_SESSION,
                             G_PARAM_READWRITE |
                             G_PARAM_CONSTRUCT_ONLY |
                             G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:auto-clipboard:
     *
     * When this is true the clipboard gets automatically shared between host
     * and guest.
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_AUTO_CLIPBOARD,
         g_param_spec_boolean("auto-clipboard",
                              "Auto clipboard",
                              "Automatically relay clipboard changes between "
                              "host and guest.",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:auto-usbredir:
     *
     * Automatically redirect newly plugged in USB devices. Note the auto
     * redirection only happens when a #SpiceDisplay associated with the
     * session had keyboard focus.
     *
     * Since: 0.8
     **/
    g_object_class_install_property
        (gobject_class, PROP_AUTO_USBREDIR,
         g_param_spec_boolean("auto-usbredir",
                              "Auto USB Redirection",
                              "Automatically redirect newly plugged in USB"
                              "Devices to the guest.",
                              FALSE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:pointer-grabbed:
     *
     * Returns %TRUE if the pointer is currently grabbed by this session.
     *
     * Since: 0.27
     **/
    g_object_class_install_property
        (gobject_class, PROP_POINTER_GRABBED,
         g_param_spec_boolean("pointer-grabbed",
                              "Pointer grabbed",
                              "Whether the pointer is grabbed",
                              FALSE,
                              G_PARAM_READABLE |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceGtkSession:sync-modifiers:
     *
     * Automatically sync modifiers (Caps, Num and Scroll locks) with the guest.
     *
     * Since: 0.32
     **/
    g_object_class_install_property
        (gobject_class, PROP_SYNC_MODIFIERS,
         g_param_spec_boolean("sync-modifiers",
                              "Sync modifiers",
                              "Automatically sync modifiers",
                              TRUE,
                              G_PARAM_READWRITE |
                              G_PARAM_CONSTRUCT |
                              G_PARAM_STATIC_STRINGS));
}

/* ---------------------------------------------------------------- */
/* private functions (clipboard related)                            */

static GdkClipboard* get_clipboard_from_selection(SpiceGtkSessionPrivate *s,
                                                   guint selection)
{
    if (selection == VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD) {
        return s->clipboard;
    } else if (selection == VD_AGENT_CLIPBOARD_SELECTION_PRIMARY) {
        return s->clipboard_primary;
    } else {
        g_warning("Unhandled clipboard selection: %u", selection);
        return NULL;
    }
}

static gint get_selection_from_clipboard(SpiceGtkSessionPrivate *s,
                                         GdkClipboard* cb)
{
    if (cb == s->clipboard) {
        return VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;
    } else if (cb == s->clipboard_primary) {
        return VD_AGENT_CLIPBOARD_SELECTION_PRIMARY;
    } else {
        g_warning("Unhandled clipboard");
        return -1;
    }
}

static const struct {
    const char  *xatom;
    uint32_t    vdagent;
} atom2agent[] = {
    {
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "UTF8_STRING",
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "text/plain;charset=utf-8"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "STRING"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "TEXT"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_UTF8_TEXT,
        .xatom   = "text/plain"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_PNG,
        .xatom   = "image/png"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-MS-bmp"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_BMP,
        .xatom   = "image/x-win-bitmap"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_TIFF,
        .xatom   = "image/tiff"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_IMAGE_JPG,
        .xatom   = "image/jpeg"
    },{
        .vdagent = VD_AGENT_CLIPBOARD_FILE_LIST,
        .xatom   = "text/uri-list"
    }
};

typedef struct {
    GWeakRef session;
    SpiceMainChannel *main;
    GdkClipboard *clipboard;
    GCancellable *cancel;
    GInputStream *stream;
    GByteArray *bytes;
    GByteArray *uri_bytes; /* retained while KDE cut-selection is read */
    gsize total_bytes;
    char *mime;
    guint selection;
    guint type;
    guint generation;
    guint8 buffer[4096];
} ClipboardRead;

static ClipboardRead *clipboard_read_new(SpiceGtkSession *self,
                                         GdkClipboard *clipboard,
                                         guint selection, guint type)
{
    ClipboardRead *read = g_new0(ClipboardRead, 1);
    g_weak_ref_init(&read->session, self);
    read->main = g_object_ref(self->priv->main);
    read->clipboard = g_object_ref(clipboard);
    read->cancel = g_cancellable_new();
    read->selection = selection;
    read->type = type;
    read->generation = self->priv->clipboard_generation[selection];
    self->priv->pending_reads = g_list_prepend(self->priv->pending_reads, read);
    return read;
}

static SpiceGtkSession *clipboard_read_get_session(ClipboardRead *read)
{
    SpiceGtkSession *self = g_weak_ref_get(&read->session);
    if (self != NULL &&
        (g_cancellable_is_cancelled(read->cancel) ||
         self->priv->main != read->main ||
         self->priv->clipboard_generation[read->selection] != read->generation ||
         !self->priv->clip_grabbed[read->selection] ||
         self->priv->clipboard_by_guest[read->selection])) {
        g_object_unref(self);
        return NULL;
    }
    return self;
}

static void clipboard_read_free(ClipboardRead *read)
{
    g_autoptr(GObject) session_ref = g_weak_ref_get(&read->session);
    SpiceGtkSession *self = (SpiceGtkSession *)session_ref;
    if (self != NULL)
        self->priv->pending_reads = g_list_remove(self->priv->pending_reads, read);
    g_weak_ref_clear(&read->session);
    g_clear_object(&read->main);
    g_clear_object(&read->clipboard);
    g_clear_object(&read->cancel);
    g_clear_object(&read->stream);
    g_clear_pointer(&read->bytes, g_byte_array_unref);
    g_clear_pointer(&read->uri_bytes, g_byte_array_unref);
    g_free(read->mime);
    g_free(read);
}

/* Cancel immediately on ownership or channel changes, even when a clipboard
 * provider is still waiting for its stream to become readable. Its callback
 * retains the request and consumes the cancelled async result. */
static void clipboard_cancel_reads(SpiceGtkSession *self, int selection)
{
    SpiceGtkSessionPrivate *s = self->priv;
    GList *l = s->pending_reads;
    while (l != NULL) {
        GList *next = l->next;
        ClipboardRead *read = l->data;
        if (selection < 0 || read->selection == (guint)selection) {
            s->pending_reads = g_list_delete_link(s->pending_reads, l);
            g_cancellable_cancel(read->cancel);
        }
        l = next;
    }
}

#ifdef HAVE_PHODAV_VIRTUAL
static SpiceWebdavChannel *clipboard_get_open_webdav(SpiceSession *session)
{
    GList *list, *l;
    SpiceChannel *channel = NULL;
    gboolean open = FALSE;

    g_return_val_if_fail(session != NULL, NULL);

    list = spice_session_get_channels(session);
    for (l = g_list_first(list); l != NULL; l = g_list_next(l)) {
        channel = l->data;

        if (!SPICE_IS_WEBDAV_CHANNEL(channel)) {
            continue;
        }

        g_object_get(channel, "port-opened", &open, NULL);
        break;
    }

    g_list_free(list);
    return open ? SPICE_WEBDAV_CHANNEL(channel) : NULL;
}

static const char* clipboard_find_atom(SpiceGtkSessionPrivate *s, guint selection, const char *a)
{
    for (int i = 0; i < s->n_atoms[selection]; i++) {
        if (g_strcmp0(s->atoms[selection][i], a) == 0) {
            return a;
        }
    }
    return NULL;
}
#endif

/* GTK4: Read content formats directly from the clipboard and process them.
 * This replaces the async gtk_clipboard_request_targets() callback pattern. */
static void clipboard_get_targets_gtk4(SpiceGtkSession *self,
                                       GdkClipboard *clipboard)
{
    SPICE_DEBUG("%s:", __FUNCTION__);

    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    SpiceGtkSessionPrivate *s = self->priv;
    guint32 types[SPICE_N_ELEMENTS(atom2agent)] = { 0 };
    gint num_types;
    int selection;

    if (s->main == NULL)
        return;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
    gsize n_mime_types = 0;
    const char * const *mime_types = gdk_content_formats_get_mime_types(formats, &n_mime_types);

    gboolean has_text = gdk_content_formats_contain_gtype(formats, G_TYPE_STRING);
    if (n_mime_types == 0 && !has_text) {
        SPICE_DEBUG("No MIME types available from clipboard");
        return;
    }

    /* Cache the MIME types as const char* array */
    g_free(s->atoms[selection]);
    s->atoms[selection] = g_new(const char*, n_mime_types);
    s->n_atoms[selection] = n_mime_types;
    for (gsize i = 0; i < n_mime_types; i++) {
        s->atoms[selection][i] = g_intern_string(mime_types[i]);
    }

    if (s->clip_grabbed[selection]) {
        SPICE_DEBUG("Clipboard is already grabbed, re-grab: %" G_GSIZE_FORMAT " MIME types", n_mime_types);
    }

    /* Set all types that match our current protocol implementation */
    num_types = 0;
    if (has_text)
        types[num_types++] = VD_AGENT_CLIPBOARD_UTF8_TEXT;
    for (gsize a = 0; a < n_mime_types; a++) {
        guint m;

        SPICE_DEBUG(" \"%s\"", mime_types[a]);

        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            guint t;

            if (strcasecmp(mime_types[a], atom2agent[m].xatom) != 0) {
                continue;
            }

            if (atom2agent[m].vdagent == VD_AGENT_CLIPBOARD_FILE_LIST) {
#ifdef HAVE_PHODAV_VIRTUAL
                if (!clipboard_get_open_webdav(s->session)) {
                    SPICE_DEBUG("Received %s target, but the clipboard webdav channel "
                                "isn't available, skipping", atom2agent[m].xatom);
                    break;
                }
#else
                break;
#endif
            }

            /* check if type is already in list */
            for (t = 0; t < num_types; t++) {
                if (types[t] == atom2agent[m].vdagent) {
                    break;
                }
            }

            if (t == num_types) {
                /* add type to empty slot */
                types[t] = atom2agent[m].vdagent;
                num_types++;
            }
        }
    }

    if (num_types == 0) {
        SPICE_DEBUG("No MIME types will be sent from %" G_GSIZE_FORMAT, n_mime_types);
        return;
    }

    s->clip_grabbed[selection] = TRUE;

    if (spice_main_channel_agent_test_capability(s->main, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND))
        spice_main_channel_clipboard_selection_grab(s->main, selection, types, num_types);

    /* Sending a grab causes the agent to do an implicit release */
    s->nclip_targets[selection] = 0;
}

/* Callback for every owner-change event for given @clipboard.
 * This event is triggered in different ways depending on the environment of
 * the Client, some examples:
 *
 * Situation 1: When another application on the client machine is holding and
 * changing the clipboard. If client is on Wayland, spice-gtk only receives the
 * related clipboard change event after focus-in event on Spice widget;
 * On X11, we will receive it at the moment the clipboard data has been
 * changed by another application.
 *
 * Situation 2: When spice-gtk holds the focus and is changing the clipboard by
 * either setting new content information with set_content or
 * clearing up old content with set_content(NULL). The main difference
 * between Wayland and X11 is that on X11, clearing sets the owner to none,
 * which emits owner-change event; On Wayland that does not happen as spice-gtk
 * still is the owner of the clipboard.
 */
static void clipboard_owner_change(GdkClipboard *clipboard,
                                   gpointer      user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    int selection;

    selection = get_selection_from_clipboard(s, clipboard);
    g_return_if_fail(selection != -1);

    s->clipboard_generation[selection]++;
    clipboard_cancel_reads(self, selection);

    if (clipboard_is_owned(self, clipboard, selection)) {
        s->clipboard_by_guest[selection] = TRUE;
        s->clip_hasdata[selection] = FALSE;
        return;
    }
    /* Local GTK widgets and other Spice sessions are host owners too. */
    s->clipboard_by_guest[selection] = FALSE;

    if (s->main == NULL) {
        return;
    }

    g_clear_pointer(&s->atoms[selection], g_free);
    s->n_atoms[selection] = 0;

    /* The current content is not this session's guest provider. */

    /* Another application now owns the clipboard */
    if (s->clip_grabbed[selection]) {
        /* We had previously grabbed on behalf of client — now it's gone */
        s->clip_grabbed[selection] = FALSE;
        if (spice_main_channel_agent_test_capability(s->main, VD_AGENT_CAP_CLIPBOARD_BY_DEMAND)) {
            spice_main_channel_clipboard_selection_release(s->main, selection);
        }
    }

    s->clip_hasdata[selection] = TRUE;

    if (s->auto_clipboard_enable && !read_only(self))
        clipboard_get_targets_gtk4(self, clipboard);
}

/* Guest clipboard data is fetched on demand when GTK asks the provider to
 * write a MIME type. Requests stay pending until a matching agent reply,
 * cancellation, disconnection, or the response deadline. */

#define SPICE_TYPE_CONTENT_PROVIDER (spice_content_provider_get_type())
G_DECLARE_FINAL_TYPE(SpiceContentProvider, spice_content_provider,
                     SPICE, CONTENT_PROVIDER, GdkContentProvider)

typedef struct ClipboardWriteRequest ClipboardWriteRequest;

struct _SpiceContentProvider {
    GdkContentProvider parent_instance;
    GWeakRef session;
    guint selection;
    GdkContentFormats *formats;
    /* Immutable snapshot: names and protocol types live in atom2agent. */
    guint mime_type_info[SPICE_N_ELEMENTS(atom2agent)];
    guint n_mime_types;
    ClipboardWriteRequest *pending_requests;
};

G_DEFINE_TYPE(SpiceContentProvider, spice_content_provider, GDK_TYPE_CONTENT_PROVIDER)

static gboolean clipboard_is_owned(SpiceGtkSession *self, GdkClipboard *clipboard,
                                    guint selection)
{
    GdkContentProvider *content = gdk_clipboard_get_content(clipboard);
    if (!SPICE_IS_CONTENT_PROVIDER(content))
        return FALSE;
    SpiceContentProvider *provider = SPICE_CONTENT_PROVIDER(content);
    g_autoptr(GObject) owner = g_weak_ref_get(&provider->session);
    g_autoptr(GObject) installed = g_weak_ref_get(&self->priv->guest_provider[selection]);
    return provider->selection == selection &&
           (owner == G_OBJECT(self) || installed == G_OBJECT(content));
}

#define CLIPBOARD_REQUEST_TIMEOUT 30 /* seconds */

struct ClipboardWriteRequest {
    ClipboardWriteRequest *next;
    guint refs;
    GTask *task;
    SpiceMainChannel *main;
    SpiceSession *session;
    GOutputStream *stream;
    GCancellable *io_cancel;
    GSource *cancel_source;
    GSource *timeout_source;
    guchar *data;
    guint selection;
    guint32 vdagent_type;
    int io_priority;
    gulong clipboard_handler;
    gulong agent_handler;
    gulong destroyed_handler;
    gboolean completed;
};

static void clipboard_write_request_unref(ClipboardWriteRequest *request)
{
    if (--request->refs != 0)
        return;

    g_clear_pointer(&request->data, g_free);
    g_clear_object(&request->io_cancel);
    g_clear_object(&request->stream);
    g_clear_object(&request->main);
    g_clear_object(&request->session);
    g_free(request);
}

static void clipboard_write_request_unlink(ClipboardWriteRequest *request)
{
    SpiceContentProvider *provider = SPICE_CONTENT_PROVIDER(
        g_task_get_source_object(request->task));
    ClipboardWriteRequest **link = &provider->pending_requests;

    while (*link && *link != request)
        link = &(*link)->next;
    if (*link)
        *link = request->next;
    request->next = NULL;
}

/* The request owns one reference until completion. An outstanding stream
 * write owns another, so cancelling can complete the task immediately while
 * the stream's callback still safely releases its result and buffer. */
static void clipboard_write_request_complete(ClipboardWriteRequest *request,
                                             GError *error)
{
    GTask *task;

    if (request->completed) {
        g_clear_error(&error);
        return;
    }

    request->refs++; /* guard against callbacks triggered by cancelling IO */
    request->completed = TRUE;
    task = request->task;
    clipboard_write_request_unlink(request);

    if (request->clipboard_handler)
        g_signal_handler_disconnect(request->main, request->clipboard_handler);
    if (request->agent_handler)
        g_signal_handler_disconnect(request->main, request->agent_handler);
    if (request->destroyed_handler)
        g_signal_handler_disconnect(request->session, request->destroyed_handler);
    if (request->cancel_source) {
        g_source_destroy(request->cancel_source);
        g_source_unref(request->cancel_source);
        request->cancel_source = NULL;
    }
    if (request->timeout_source) {
        g_source_destroy(request->timeout_source);
        g_source_unref(request->timeout_source);
        request->timeout_source = NULL;
    }

    if (error) {
        g_cancellable_cancel(request->io_cancel);
        g_task_return_error(task, error);
    } else {
        g_task_return_boolean(task, TRUE);
    }
    request->task = NULL;
    g_object_unref(task);
    clipboard_write_request_unref(request); /* pending request */
    clipboard_write_request_unref(request); /* reentrancy guard */
}

static void clipboard_write_stream_done(GObject *stream, GAsyncResult *result,
                                        gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;
    gsize bytes_written;
    GError *error = NULL;

    if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(stream), result,
                                          &bytes_written, &error) && !error)
        error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not write guest clipboard data");
    if (!request->completed)
        clipboard_write_request_complete(request, error);
    else
        g_clear_error(&error);
    clipboard_write_request_unref(request); /* outstanding stream write */
}

static void clipboard_got_from_guest_gtk4(SpiceMainChannel *main,
                                          guint selection, guint type,
                                          const guchar *data, guint size,
                                          gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;

    if (request->completed || selection != request->selection ||
        type != request->vdagent_type || request->clipboard_handler == 0)
        return;

    g_signal_handler_disconnect(main, request->clipboard_handler);
    clipboard_write_request_unlink(request);
    request->clipboard_handler = 0;
    /* The deadline applies only to the guest response, not to stream IO. */
    g_source_destroy(request->timeout_source);
    g_source_unref(request->timeout_source);
    request->timeout_source = NULL;

    if (!data || !size) {
        clipboard_write_request_complete(request,
            g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                        "No data received from guest"));
        return;
    }

    gsize write_len = size;
    if (type == VD_AGENT_CLIPBOARD_UTF8_TEXT &&
        spice_main_channel_agent_test_capability(main, VD_AGENT_CAP_GUEST_LINEEND_CRLF)) {
        request->data = (guchar *)spice_dos2unix((const gchar *)data, size);
        write_len = strlen((const gchar *)request->data);
    } else {
        request->data = g_memdup2(data, size);
    }

    request->refs++; /* retained until write_all_finish, even on cancellation */
    g_output_stream_write_all_async(request->stream, request->data, write_len,
                                    request->io_priority, request->io_cancel,
                                    clipboard_write_stream_done, request);
}

static gboolean clipboard_write_cancelled(GCancellable *cancellable G_GNUC_UNUSED,
                                          gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;
    clipboard_write_request_complete(request,
        g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Clipboard write cancelled"));
    return G_SOURCE_REMOVE;
}

static gboolean clipboard_write_timed_out(gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;
    clipboard_write_request_complete(request,
        g_error_new(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Guest clipboard response timed out"));
    return G_SOURCE_REMOVE;
}

static void clipboard_channel_destroyed(SpiceSession *session G_GNUC_UNUSED,
                                        SpiceChannel *channel, gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;
    if (channel == SPICE_CHANNEL(request->main))
        clipboard_write_request_complete(request,
            g_error_new(G_IO_ERROR, G_IO_ERROR_CLOSED, "Clipboard channel destroyed"));
}

static void clipboard_agent_connected_gtk4(SpiceMainChannel *main,
                                           GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ClipboardWriteRequest *request = user_data;
    gboolean connected;

    g_object_get(main, "agent-connected", &connected, NULL);
    if (!connected)
        clipboard_write_request_complete(request,
            g_error_new(G_IO_ERROR, G_IO_ERROR_CLOSED, "Clipboard agent disconnected"));
}

static void
spice_content_provider_write_mime_type_async(GdkContentProvider *provider,
                                             const char         *mime_type,
                                             GOutputStream      *stream,
                                             int                 io_priority,
                                             GCancellable       *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer            user_data)
{
    SpiceContentProvider *self = SPICE_CONTENT_PROVIDER(provider);
    SpiceGtkSessionPrivate *s;
    GTask *task;
    guint info_index = G_MAXUINT;
    int selection;

    task = g_task_new(provider, cancellable, callback, user_data);
    g_autoptr(GObject) session_ref = g_weak_ref_get(&self->session);
    SpiceGtkSession *session = (SpiceGtkSession *)session_ref;

    if (session == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "SpiceGtkSession is gone");
        g_object_unref(task);
        return;
    }

    s = session->priv;

    /* Find which atom2agent entry this MIME type corresponds to */
    for (guint i = 0; i < self->n_mime_types; i++) {
        if (g_strcmp0(mime_type, atom2agent[self->mime_type_info[i]].xatom) == 0) {
            info_index = self->mime_type_info[i];
            break;
        }
    }

    if (info_index == G_MAXUINT || info_index >= SPICE_N_ELEMENTS(atom2agent)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Unsupported MIME type: %s", mime_type);
        g_object_unref(task);
        return;
    }

    /* Determine which clipboard selection this provider is on */
    for (guint sel = 0; sel < CLIPBOARD_LAST; sel++) {
        GdkClipboard *cb = get_clipboard_from_selection(s, sel);
        if (cb && gdk_clipboard_get_content(cb) == provider) {
            selection = sel;
            break;
        }
        if (sel == CLIPBOARD_LAST - 1) {
            g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                    "Could not determine clipboard selection");
            g_object_unref(task);
            return;
        }
    }

    if (s->main == NULL || s->session == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "Clipboard channel is gone");
        g_object_unref(task);
        return;
    }

    if (s->clipboard_release_delay[selection]) {
        SPICE_DEBUG("not requesting data from guest during delayed release");
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Clipboard release pending");
        g_object_unref(task);
        return;
    }

    if (g_task_return_error_if_cancelled(task)) {
        g_object_unref(task);
        return;
    }

    gboolean agent_connected;
    g_object_get(s->main, "agent-connected", &agent_connected, NULL);
    if (!agent_connected) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                "Clipboard agent not connected");
        g_object_unref(task);
        return;
    }

    ClipboardWriteRequest *request = g_new0(ClipboardWriteRequest, 1);
    request->refs = 1;
    request->task = task;
    request->main = g_object_ref(s->main);
    request->session = g_object_ref(s->session);
    request->stream = g_object_ref(stream);
    request->io_cancel = g_cancellable_new();
    request->selection = selection;
    request->vdagent_type = atom2agent[info_index].vdagent;
    request->io_priority = io_priority;

    request->clipboard_handler = g_signal_connect(request->main,
        "main-clipboard-selection", G_CALLBACK(clipboard_got_from_guest_gtk4), request);
    request->agent_handler = g_signal_connect(request->main,
        "notify::agent-connected", G_CALLBACK(clipboard_agent_connected_gtk4), request);
    request->destroyed_handler = g_signal_connect(request->session,
        "channel-destroy", G_CALLBACK(clipboard_channel_destroyed), request);

    if (cancellable) {
        request->cancel_source = g_cancellable_source_new(cancellable);
        g_source_set_callback(request->cancel_source,
                              G_SOURCE_FUNC(clipboard_write_cancelled), request, NULL);
        g_source_attach(request->cancel_source, g_task_get_context(task));
    }

    request->timeout_source = g_timeout_source_new_seconds(CLIPBOARD_REQUEST_TIMEOUT);
    g_source_set_callback(request->timeout_source, clipboard_write_timed_out,
                          request, NULL);
    g_source_attach(request->timeout_source, g_task_get_context(task));

    /* Replies carry only selection and type, not a request ID. Share one
     * guest request among overlapping writes of the same pair, while each
     * consumer retains its own task, stream, and cancellation lifetime. */
    gboolean pending = FALSE;
    for (ClipboardWriteRequest *other = self->pending_requests; other;
         other = other->next) {
        if (other->main == request->main && other->selection == selection &&
            other->vdagent_type == request->vdagent_type) {
            pending = TRUE;
            break;
        }
    }
    request->next = self->pending_requests;
    self->pending_requests = request;
    if (!pending)
        spice_main_channel_clipboard_selection_request(request->main, selection,
                                                       request->vdagent_type);
}

static gboolean
spice_content_provider_write_mime_type_finish(GdkContentProvider *provider,
                                              GAsyncResult       *result,
                                              GError            **error)
{
    return g_task_propagate_boolean(G_TASK(result), error);
}

static GdkContentFormats *
spice_content_provider_ref_formats(GdkContentProvider *provider)
{
    SpiceContentProvider *self = SPICE_CONTENT_PROVIDER(provider);
    if (self->formats)
        return gdk_content_formats_ref(self->formats);
    return gdk_content_formats_new(NULL, 0);
}

static void
spice_content_provider_finalize(GObject *object)
{
    SpiceContentProvider *self = SPICE_CONTENT_PROVIDER(object);
    g_weak_ref_clear(&self->session);

    g_clear_pointer(&self->formats, gdk_content_formats_unref);

    G_OBJECT_CLASS(spice_content_provider_parent_class)->finalize(object);
}

static void
spice_content_provider_class_init(SpiceContentProviderClass *klass)
{
    GdkContentProviderClass *provider_class = GDK_CONTENT_PROVIDER_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    provider_class->write_mime_type_async = spice_content_provider_write_mime_type_async;
    provider_class->write_mime_type_finish = spice_content_provider_write_mime_type_finish;
    provider_class->ref_formats = spice_content_provider_ref_formats;

    object_class->finalize = spice_content_provider_finalize;
}

static void
spice_content_provider_init(SpiceContentProvider *self G_GNUC_UNUSED)
{
}

static GdkContentProvider *
spice_content_provider_new(SpiceGtkSession *session,
                           guint            selection,
                           const guint     *info_indices,
                           guint            n_types)
{
    SpiceContentProvider *provider = g_object_new(SPICE_TYPE_CONTENT_PROVIDER, NULL);
    g_weak_ref_init(&provider->session, session);
    const char *mime_types[SPICE_N_ELEMENTS(atom2agent)];
    provider->selection = selection;
    for (guint i = 0; i < n_types; i++) {
        provider->mime_type_info[i] = info_indices[i];
        mime_types[i] = atom2agent[info_indices[i]].xatom;
    }
    provider->formats = gdk_content_formats_new(mime_types, n_types);
    provider->n_mime_types = n_types;
    return GDK_CONTENT_PROVIDER(provider);
}

static gboolean clipboard_grab(SpiceMainChannel *main, guint selection,
                               guint32* types, guint32 ntypes,
                               gpointer user_data)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(user_data), FALSE);

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    gboolean target_selected[SPICE_N_ELEMENTS(atom2agent)] = { FALSE, };
    gboolean found;
    GdkClipboard *cb;
    int m, n;
    int num_targets = 0;

    guint info_indices[SPICE_N_ELEMENTS(atom2agent)];

    clipboard_release_delay_remove(self, selection, false);

    cb = get_clipboard_from_selection(s, selection);
    g_return_val_if_fail(cb != NULL, FALSE);
    clipboard_cancel_reads(self, selection);
    s->clipboard_generation[selection]++;

    for (n = 0; n < ntypes; ++n) {
        found = FALSE;
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (atom2agent[m].vdagent == types[n] && !target_selected[m]) {
                found = TRUE;
                g_return_val_if_fail(num_targets < SPICE_N_ELEMENTS(atom2agent), FALSE);
                info_indices[num_targets] = m;
                target_selected[m] = TRUE;
                num_targets++;
            }
        }
        if (!found) {
            g_warning("clipboard: couldn't find a matching type for: %u",
                      types[n]);
        }
    }

    s->nclip_targets[selection] = num_targets;
    g_free(s->clip_target_info[selection]);
    s->clip_target_info[selection] = g_memdup2(info_indices, sizeof(guint) * num_targets);

    /* Receiving a grab implies we've released our own grab */
    s->clip_grabbed[selection] = FALSE;

    if (read_only(self) ||
        !s->auto_clipboard_enable ||
        s->nclip_targets[selection] == 0) {
        return TRUE;
    }

    {
        GdkContentProvider *provider;
        provider = spice_content_provider_new(self, selection, info_indices, num_targets);
        if (!gdk_clipboard_set_content(cb, provider)) {
            g_warning("clipboard grab failed");
            g_object_unref(provider);
            return FALSE;
        }
        g_weak_ref_set(&s->guest_provider[selection], provider);
        g_object_unref(provider);
    }
    s->clipboard_by_guest[selection] = clipboard_is_owned(self, cb, selection);
    s->clip_hasdata[selection] = !s->clipboard_by_guest[selection];

    return TRUE;
}

static gboolean check_clipboard_size_limits(SpiceGtkSession *session,
                                            gint clipboard_len)
{
    int max_clipboard;

    g_object_get(session->priv->main, "max-clipboard", &max_clipboard, NULL);
    if (max_clipboard != -1 && clipboard_len > max_clipboard) {
        g_warning("discarded clipboard of size %d (max: %d)",
                  clipboard_len, max_clipboard);
        return FALSE;
    } else if (clipboard_len <= 0) {
        SPICE_DEBUG("discarding empty clipboard");
        return FALSE;
    }

    return TRUE;
}

/* This will convert line endings if needed (between Windows/Unix conventions),
 * and will make sure 'len' does not take into account any trailing \0 as this could
 * cause some confusion guest side.
 * The 'len' argument will be modified by this function to the length of the modified
 * string
 */
static char *fixup_clipboard_text(SpiceGtkSession *self, const char *text, int *len)
{
    char *conv = NULL;

    if (spice_main_channel_agent_test_capability(self->priv->main,
                                                 VD_AGENT_CAP_GUEST_LINEEND_CRLF)) {
        conv = spice_unix2dos(text, *len);
        *len = strlen(conv);
    } else {
        /* On Windows, with some versions of gtk+, GtkSelectionData::length
         * will include the final '\0'. When a string with this trailing '\0'
         * is pasted in some linux applications, it will be pasted as <NIL> or
         * as an invisible character, which is unwanted. Ensure the length we
         * send to the agent does not include any trailing '\0'
         * This is gtk+ bug https://bugzilla.gnome.org/show_bug.cgi?id=734670
         */
        *len = strlen(text);
    }

    return conv;
}

static void clipboard_received_text_cb(GObject *source, GAsyncResult *result,
                                       gpointer user_data)
{
    ClipboardRead *read = user_data;
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, NULL);
    g_autoptr(GObject) session_ref = (GObject *)clipboard_read_get_session(read);
    SpiceGtkSession *self = (SpiceGtkSession *)session_ref;
    g_autofree char *conv = NULL;
    const guchar *data = NULL;
    int len = 0;

    if (self == NULL)
        goto done;
    if (text != NULL && strlen(text) <= G_MAXINT) {
        len = strlen(text);
        if (check_clipboard_size_limits(self, len)) {
            conv = fixup_clipboard_text(self, text, &len);
            if (check_clipboard_size_limits(self, len))
                data = (const guchar *)(conv != NULL ? conv : text);
        }
    }
    spice_main_channel_clipboard_selection_notify(read->main, read->selection,
                                                  read->type, data,
                                                  data != NULL ? len : 0);
done:
    clipboard_read_free(read);
}

static void clipboard_stream_opened_cb(GObject *source, GAsyncResult *result,
                                       gpointer user_data);

#ifdef HAVE_PHODAV_VIRTUAL
/* returns path to @file under @root in clipboard phodav server, or NULL on error */
static gchar *clipboard_webdav_share_file(PhodavVirtualDir *root, GFile *file)
{
    gchar *uuid;
    PhodavVirtualDir *dir;
    GError *err = NULL;

    /* separate directory is created for each file,
     * as we want to preserve the original filename and avoid conflicts */
    for (guint i = 0; i < 8; i++) {
        uuid = g_uuid_string_random();
        gchar *dir_path = g_strdup_printf(SPICE_WEBDAV_CLIPBOARD_FOLDER_PATH "/%s", uuid);
        dir = phodav_virtual_dir_new_dir(root, dir_path, &err);
        g_free(dir_path);
        if (!err) {
            break;
        }
        g_clear_pointer(&uuid, g_free);
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
            g_warning("failed to create phodav virtual dir: %s", err->message);
            g_error_free(err);
            return NULL;
        }
        g_clear_error(&err);
    }

    if (!dir) {
        g_warning("failed to create phodav virtual dir: all attempts failed");
        return NULL;
    }

    phodav_virtual_dir_attach_real_child(dir, file);
    g_object_unref(dir);

    gchar *base = g_file_get_basename(file);
    gchar *path = g_strdup_printf(SPICE_WEBDAV_CLIPBOARD_FOLDER_PATH "/%s/%s", uuid, base);
    g_free(uuid);
    g_free(base);

    return path;
}

/* join all strings in @strv into a new char array,
 * including all terminating NULL-chars */
static gchar *strv_concat(gchar **strv, gsize *size_out)
{
    gchar **str_p, *arr, *curr;

    g_return_val_if_fail(strv && size_out, NULL);

    for (str_p = strv, *size_out = 0; *str_p != NULL; str_p++) {
        *size_out += strlen(*str_p) + 1;
    }

    arr = g_malloc(*size_out);

    for (str_p = strv, curr = arr; *str_p != NULL; str_p++) {
        curr = g_stpcpy(curr, *str_p) + 1;
    }

    return arr;
}

/* if not done alreay, share all files in @uris using the webdav server
 * and return a new buffer with VD_AGENT_CLIPBOARD_FILE_LIST data */
static gchar *strv_uris_transform_to_data(SpiceGtkSessionPrivate *s,
    gchar **uris, gsize *size_out, GdkDragAction action)
{
    SpiceWebdavChannel *webdav;
    /* if there's version mismatch between spice-client-gtk and spice-client-glib,
     * "webdav-server" property might not be present, so phodav must be initialized to NULL */
    PhodavServer *phodav = NULL;
    PhodavVirtualDir *root;

    gchar **uri_ptr, *path, **paths, *data;
    GFile *file;
    guint n;

    *size_out = 0;

    if (!uris || g_strv_length(uris) < 1) {
        return NULL;
    }

    webdav = clipboard_get_open_webdav(s->session);
    if (!webdav) {
        SPICE_DEBUG("Received uris, but no webdav channel");
        return NULL;
    }

    g_object_get(s->session, "webdav-server", &phodav, NULL);
    if (!phodav) {
        return NULL;
    }
    g_object_get(phodav, "root-file", &root, NULL);
    g_object_unref(phodav);

    paths = g_new0(gchar *, g_strv_length(uris) + 2);

    paths[0] = action == GDK_ACTION_MOVE ? "cut" : "copy";
    n = 1;

    for (uri_ptr = uris; *uri_ptr != NULL; uri_ptr++) {
        file = g_file_new_for_uri(*uri_ptr);

        /* clipboard data is usually requested multiple times for no obvious reasons
         * (clipboar managers to blame?), we don't want to create multiple dirs for the same file */
        path = g_hash_table_lookup(s->cb_shared_files, file);
        if (path) {
            SPICE_DEBUG("found %s with path %s", *uri_ptr, path);
            g_object_unref(file);
        } else {
            path = clipboard_webdav_share_file(root, file);
            g_return_val_if_fail(path != NULL, NULL);
            SPICE_DEBUG("publishing %s under %s", *uri_ptr, path);
            /* file and path gets freed once the hash table gets destroyed */
            g_hash_table_insert(s->cb_shared_files, file, path);
        }
        paths[n] = path;
        n++;
    }

    g_object_unref(root);
    data = strv_concat(paths, size_out);
    g_free(paths);

    return data;
}

static const char *a_gnome, *a_mate, *a_nautilus, *a_uri_list, *a_kde_cut;

static void init_uris_atoms()
{
    if (a_gnome != NULL) {
        return;
    }
    a_gnome = "x-special/gnome-copied-files";
    a_mate = "x-special/mate-copied-files";
    a_nautilus = "UTF8_STRING";
    a_uri_list = "text/uri-list";
    a_kde_cut = "application/x-kde-cutselection";
}

static const char* clipboard_select_uris_atom(SpiceGtkSessionPrivate *s, guint selection)
{
    init_uris_atoms();
    if (clipboard_find_atom(s, selection, a_gnome)) {
        return a_gnome;
    }
    if (clipboard_find_atom(s, selection, a_mate)) {
        return a_mate;
    }
    if (clipboard_find_atom(s, selection, a_nautilus)) {
        return a_nautilus;
    }
    return clipboard_find_atom(s, selection, a_uri_list);
}

/* common handler for "x-special/gnome-copied-files" and "x-special/mate-copied-files" */
static gchar *x_special_copied_files_transform_to_data(SpiceGtkSessionPrivate *s,
    const gchar *text, gsize *size_out)
{
    gchar **lines, *data = NULL;
    GdkDragAction action;

    *size_out = 0;

    if (!text) {
        return NULL;
    }
    lines = g_strsplit(text, "\n", -1);
    if (g_strv_length(lines) < 2) {
        goto err;
    }

    if (!g_strcmp0(lines[0], "cut")) {
        action = GDK_ACTION_MOVE;
    } else if (!g_strcmp0(lines[0], "copy")) {
        action = GDK_ACTION_COPY;
    } else {
        goto err;
    }

    data = strv_uris_transform_to_data(s, &lines[1], size_out, action);
err:
    g_strfreev(lines);
    return data;
}

/* used with newer Nautilus */
static gchar *nautilus_uris_transform_to_data(SpiceGtkSessionPrivate *s,
    const gchar *raw_text, gsize *size_out, gboolean *retry_out)
{
    gchar **lines, *data = NULL;
    guint n_lines;
    GdkDragAction action;

    *size_out = 0;

    if (!raw_text) {
        return NULL;
    }
    lines = g_strsplit(raw_text, "\n", -1);
    n_lines = g_strv_length(lines);

    if (n_lines < 4) {
        *retry_out = TRUE;
        goto err;
    }

    if (g_strcmp0(lines[0], "x-special/nautilus-clipboard")) {
        *retry_out = TRUE;
        goto err;
    }

    if (!g_strcmp0(lines[1], "cut")) {
        action = GDK_ACTION_MOVE;
    } else if (!g_strcmp0(lines[1], "copy")) {
        action = GDK_ACTION_COPY;
    } else {
        goto err;
    }

    /* the list of uris must end with \n,
     * so there must be an empty string after the split */
    if (g_strcmp0(lines[n_lines-1], "")) {
        goto err;
    }
    g_clear_pointer(&lines[n_lines-1], g_free);

    data = strv_uris_transform_to_data(s, &lines[2], size_out, action);
err:
    g_strfreev(lines);
    return data;
}

/* Returns TRUE when Nautilus fallback or KDE metadata needs another read. */
static gboolean clipboard_process_uri_contents(ClipboardRead *read, SpiceGtkSession *self)
{
    SpiceGtkSessionPrivate *s = self->priv;
    gchar *data = NULL;
    gsize len = 0;
    const gchar *text;

    g_byte_array_append(read->bytes, (const guint8 *)"", 1);
    text = (const gchar *)read->bytes->data;
    init_uris_atoms();

    if (g_strcmp0(read->mime, "x-special/gnome-copied-files") == 0 ||
        g_strcmp0(read->mime, "x-special/mate-copied-files") == 0) {
        data = x_special_copied_files_transform_to_data(s, text, &len);
    } else if (g_strcmp0(read->mime, "UTF8_STRING") == 0) {
        gboolean retry = FALSE;
        data = nautilus_uris_transform_to_data(s, text, &len, &retry);
        if (retry && clipboard_find_atom(s, read->selection, a_uri_list) != NULL) {
            const char *mimes[] = { "text/uri-list", NULL };
            g_clear_object(&read->stream);
            g_clear_pointer(&read->bytes, g_byte_array_unref);
            g_clear_pointer(&read->mime, g_free);
            gdk_clipboard_read_async(read->clipboard, mimes, G_PRIORITY_DEFAULT,
                                     read->cancel, clipboard_stream_opened_cb, read);
            return TRUE;
        }
    } else if (g_strcmp0(read->mime, "text/uri-list") == 0 || read->uri_bytes != NULL) {
        GdkDragAction action = GDK_ACTION_COPY;
        if (read->uri_bytes != NULL) {
            if (g_strcmp0(text, "1") == 0)
                action = GDK_ACTION_MOVE;
            text = (const gchar *)read->uri_bytes->data;
        } else if (clipboard_find_atom(s, read->selection, a_kde_cut) != NULL) {
            const char *mimes[] = { "application/x-kde-cutselection", NULL };
            read->uri_bytes = g_steal_pointer(&read->bytes);
            g_clear_object(&read->stream);
            g_clear_pointer(&read->mime, g_free);
            gdk_clipboard_read_async(read->clipboard, mimes, G_PRIORITY_DEFAULT,
                                     read->cancel, clipboard_stream_opened_cb, read);
            return TRUE;
        }
        gchar **uris = g_strsplit(text, "\n", -1);
        guint uri_count = 0;
        for (guint i = 0; uris[i] != NULL; i++) {
            g_strstrip(uris[i]);
            if (uris[i][0] != '\0' && uris[i][0] != '#')
                uris[uri_count++] = uris[i];
            else
                g_free(uris[i]);
        }
        uris[uri_count] = NULL;
        data = strv_uris_transform_to_data(s, uris, &len, action);
        g_strfreev(uris);
    }

    if (data != NULL && (len > G_MAXINT || !check_clipboard_size_limits(self, len))) {
        g_clear_pointer(&data, g_free);
        len = 0;
    }
    spice_main_channel_clipboard_selection_notify(read->main, read->selection,
                                                   read->type, (guchar *)data, len);
    g_free(data);
    return FALSE;
}

#endif /* HAVE_PHODAV_VIRTUAL */

static void clipboard_stream_bytes_cb(GObject *source, GAsyncResult *result,
                                      gpointer user_data)
{
    ClipboardRead *read = user_data;
    g_autoptr(GError) error = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);
    g_autoptr(GObject) session_ref = (GObject *)clipboard_read_get_session(read);
    SpiceGtkSession *self = (SpiceGtkSession *)session_ref;
    gint max_clipboard;

    if (self == NULL)
        goto done;
    if (n < 0) {
        SPICE_DEBUG("Failed to read clipboard stream: %s", error->message);
        goto failed;
    }

    if (n != 0) {
        g_object_get(read->main, "max-clipboard", &max_clipboard, NULL);
        if ((gsize)n > (gsize)G_MAXINT - read->total_bytes ||
            (max_clipboard >= 0 && read->total_bytes + (gsize)n > (gsize)max_clipboard)) {
            SPICE_DEBUG("Host clipboard exceeds guest clipboard limit");
            goto failed;
        }
        g_byte_array_append(read->bytes, read->buffer, n);
        read->total_bytes += n;
        g_input_stream_read_async(read->stream, read->buffer, sizeof(read->buffer),
                                  G_PRIORITY_DEFAULT, read->cancel,
                                  clipboard_stream_bytes_cb, read);
        return;
    }

#ifdef HAVE_PHODAV_VIRTUAL
    if (read->type == VD_AGENT_CLIPBOARD_FILE_LIST) {
        if (clipboard_process_uri_contents(read, self))
            return;
        goto done;
    }
#endif
    if (check_clipboard_size_limits(self, read->bytes->len)) {
        for (guint m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (strcasecmp(read->mime, atom2agent[m].xatom) == 0 &&
                atom2agent[m].vdagent == read->type) {
                spice_main_channel_clipboard_selection_notify(read->main,
                    read->selection, read->type, read->bytes->data, read->bytes->len);
                goto done;
            }
        }
    }
failed:
    spice_main_channel_clipboard_selection_notify(read->main, read->selection,
                                                   read->type, NULL, 0);
done:
    clipboard_read_free(read);
}

static void clipboard_stream_opened_cb(GObject *source, GAsyncResult *result,
                                       gpointer user_data)
{
    ClipboardRead *read = user_data;
    const char *out_mime = NULL;
    g_autoptr(GError) error = NULL;
    read->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), result,
                                              &out_mime, &error);
    g_autoptr(GObject) session_ref = (GObject *)clipboard_read_get_session(read);
    if (session_ref == NULL)
        goto done;
    if (read->stream == NULL || out_mime == NULL) {
        SPICE_DEBUG("Failed to open clipboard stream: %s",
                    error ? error->message : "unknown");
        spice_main_channel_clipboard_selection_notify(read->main, read->selection,
                                                       read->type, NULL, 0);
        goto done;
    }

    read->mime = g_strdup(out_mime);
    read->bytes = g_byte_array_new();
    g_input_stream_read_async(read->stream, read->buffer, sizeof(read->buffer),
                              G_PRIORITY_DEFAULT, read->cancel,
                              clipboard_stream_bytes_cb, read);
    return;
done:
    clipboard_read_free(read);
}

static gboolean clipboard_request(SpiceMainChannel *main, guint selection,
                                  guint type, gpointer user_data)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(user_data), FALSE);

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    GdkClipboard *cb;
    int m;

    cb = get_clipboard_from_selection(s, selection);
    g_return_val_if_fail(cb != NULL, FALSE);
    g_return_val_if_fail(s->clipboard_by_guest[selection] == FALSE, FALSE);
    g_return_val_if_fail(s->clip_grabbed[selection], FALSE);

    if (read_only(self))
        return FALSE;

    if (type == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        ClipboardRead *read = clipboard_read_new(self, cb, selection, type);
        gdk_clipboard_read_text_async(cb, read->cancel,
                                      clipboard_received_text_cb, read);
    } else if (type == VD_AGENT_CLIPBOARD_FILE_LIST) {
#ifdef HAVE_PHODAV_VIRTUAL
        const char *atom = clipboard_select_uris_atom(s, selection);
        if (atom == NULL)
            return FALSE;
        ClipboardRead *read = clipboard_read_new(self, cb, selection, type);
        const char *mimes[] = { atom, NULL };
        gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT,
                                 read->cancel, clipboard_stream_opened_cb, read);
#else
        return FALSE;
#endif
    } else {
        for (m = 0; m < SPICE_N_ELEMENTS(atom2agent); m++) {
            if (atom2agent[m].vdagent == type)
                break;
        }

        g_return_val_if_fail(m < SPICE_N_ELEMENTS(atom2agent), FALSE);
        ClipboardRead *read = clipboard_read_new(self, cb, selection, type);
        const char *mimes[] = { atom2agent[m].xatom, NULL };
        gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT,
                                 read->cancel, clipboard_stream_opened_cb, read);
    }

    return TRUE;
}

static void clipboard_release(SpiceGtkSession *self, guint selection)
{
    SpiceGtkSessionPrivate *s = self->priv;
    GdkClipboard *clipboard = get_clipboard_from_selection(s, selection);

    g_return_if_fail(clipboard != NULL);

    s->nclip_targets[selection] = 0;

    s->clipboard_by_guest[selection] = FALSE;
    if (clipboard_is_owned(self, clipboard, selection))
        gdk_clipboard_set_content(clipboard, NULL);
}

typedef struct SpiceGtkClipboardRelease {
    SpiceGtkSession *self;
    guint selection;
} SpiceGtkClipboardRelease;

static gboolean clipboard_release_timeout(gpointer user_data)
{
    SpiceGtkClipboardRelease *rel = user_data;

    rel->self->priv->clipboard_release_delay[rel->selection] = 0;
    clipboard_release(rel->self, rel->selection);

    return G_SOURCE_REMOVE;
}

/*
 * The agents send release between two grabs. This may trigger
 * clipboard managers trying to grab the clipboard. We end up with two
 * sides, client and remote, racing for the clipboard grab, and
 * believing each other is the owner.
 *
 * Workaround this problem by delaying the release event by 0.5 sec,
 * unless the no-release-on-regrab capability is present.
 */
#define CLIPBOARD_RELEASE_DELAY 500 /* ms */

static void clipboard_release_delay(SpiceMainChannel *main, guint selection,
                                    gpointer user_data)
{
    SpiceGtkSession *self = SPICE_GTK_SESSION(user_data);
    SpiceGtkSessionPrivate *s = self->priv;
    GdkClipboard *clipboard = get_clipboard_from_selection(s, selection);
    SpiceGtkClipboardRelease *rel;

    if (!clipboard) {
        return;
    }

    clipboard_release_delay_remove(self, selection, true);

    if (spice_main_channel_agent_test_capability(s->main,
                                                 VD_AGENT_CAP_CLIPBOARD_NO_RELEASE_ON_REGRAB)) {
        clipboard_release(self, selection);
        return;
    }

    rel = g_new0(SpiceGtkClipboardRelease, 1);
    rel->self = self;
    rel->selection = selection;
    s->clipboard_release_delay[selection] =
        g_timeout_add_full(G_PRIORITY_DEFAULT, CLIPBOARD_RELEASE_DELAY,
                           clipboard_release_timeout, rel, g_free);

}

static void channel_new(SpiceSession *session, SpiceChannel *channel,
                        gpointer user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        SPICE_DEBUG("Changing main channel from %p to %p", s->main, channel);
        if (s->main && s->main != SPICE_MAIN_CHANNEL(channel)) {
            clipboard_cancel_reads(self, -1);
            g_signal_handlers_disconnect_by_data(s->main, self);
        }
        s->main = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "main-clipboard-selection-grab",
                         G_CALLBACK(clipboard_grab), self);
        g_signal_connect(channel, "main-clipboard-selection-request",
                         G_CALLBACK(clipboard_request), self);
        g_signal_connect(channel, "main-clipboard-selection-release",
                         G_CALLBACK(clipboard_release_delay), self);
    }
    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        spice_g_signal_connect_object(channel, "inputs-modifiers",
                                      G_CALLBACK(guest_modifiers_changed), self, 0);
        spice_gtk_session_sync_keyboard_modifiers_for_channel(self, SPICE_INPUTS_CHANNEL(channel), TRUE);
    }
}

static void channel_destroy(SpiceSession *session, SpiceChannel *channel,
                            gpointer user_data)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(user_data));

    SpiceGtkSession *self = user_data;
    SpiceGtkSessionPrivate *s = self->priv;
    guint i;

    if (SPICE_IS_MAIN_CHANNEL(channel) && SPICE_MAIN_CHANNEL(channel) == s->main) {
        clipboard_cancel_reads(self, -1);
        g_signal_handlers_disconnect_by_data(channel, self);
        for (i = 0; i < CLIPBOARD_LAST; ++i)
            clipboard_release_delay_remove(self, i, FALSE);
        s->main = NULL;
        for (i = 0; i < CLIPBOARD_LAST; ++i) {
            if (s->clipboard_by_guest[i]) {
                GdkClipboard *cb = get_clipboard_from_selection(s, i);
                if (cb && clipboard_is_owned(self, cb, i))
                    gdk_clipboard_set_content(cb, NULL);
                s->clipboard_by_guest[i] = FALSE;
            }
            s->clip_grabbed[i] = FALSE;
            s->nclip_targets[i] = 0;
        }
    }
}

static gboolean read_only(SpiceGtkSession *self)
{
    return spice_session_get_read_only(self->priv->session);
}

/* ---------------------------------------------------------------- */
/* private functions (usbredir related)                             */
G_GNUC_INTERNAL
void spice_gtk_session_request_auto_usbredir(SpiceGtkSession *self,
                                             gboolean state)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    SpiceGtkSessionPrivate *s = self->priv;
    SpiceDesktopIntegration *desktop_int;
    SpiceUsbDeviceManager *manager;

    if (state) {
        s->auto_usbredir_reqs++;
        if (s->auto_usbredir_reqs != 1)
            return;
    } else {
        g_return_if_fail(s->auto_usbredir_reqs > 0);
        s->auto_usbredir_reqs--;
        if (s->auto_usbredir_reqs != 0)
            return;
    }

    if (!s->auto_usbredir_enable)
        return;

    manager = spice_usb_device_manager_get(s->session, NULL);
    if (!manager)
        return;

    g_object_set(manager, "auto-connect", state, NULL);

    desktop_int = spice_desktop_integration_get(s->session);
    if (state)
        spice_desktop_integration_inhibit_automount(desktop_int);
    else
        spice_desktop_integration_uninhibit_automount(desktop_int);
}

/* ------------------------------------------------------------------ */
/* public functions                                                   */

/**
 * spice_gtk_session_get:
 * @session: #SpiceSession for which to get the #SpiceGtkSession
 *
 * Gets the #SpiceGtkSession associated with the passed in #SpiceSession.
 * A new #SpiceGtkSession instance will be created the first time this
 * function is called for a certain #SpiceSession.
 *
 * Note that this function returns a weak reference, which should not be used
 * after the #SpiceSession itself has been unref-ed by the caller.
 *
 * Returns: (transfer none): a weak reference to the #SpiceGtkSession associated with the passed in #SpiceSession
 *
 * Since 0.8
 **/
SpiceGtkSession *spice_gtk_session_get(SpiceSession *session)
{
    g_return_val_if_fail(SPICE_IS_SESSION(session), NULL);

    SpiceGtkSession *self;
    static GMutex mutex;

    g_mutex_lock(&mutex);
    self = g_object_get_data(G_OBJECT(session), "spice-gtk-session");
    if (self == NULL) {
        self = g_object_new(SPICE_TYPE_GTK_SESSION, "session", session, NULL);
        g_object_set_data_full(G_OBJECT(session), "spice-gtk-session", self, g_object_unref);
    }
    g_mutex_unlock(&mutex);

    return SPICE_GTK_SESSION(self);
}

/**
 * spice_gtk_session_copy_to_guest:
 * @self: #SpiceGtkSession
 *
 * Copy client-side clipboard to guest clipboard.
 *
 * Since 0.8
 **/
void spice_gtk_session_copy_to_guest(SpiceGtkSession *self)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));
    g_return_if_fail(read_only(self) == FALSE);

    SpiceGtkSessionPrivate *s = self->priv;
    int selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (s->clip_hasdata[selection] && !s->clip_grabbed[selection]) {
        /* GTK4: read clipboard formats directly instead of requesting targets */
        clipboard_get_targets_gtk4(self, s->clipboard);
    }
}

/**
 * spice_gtk_session_paste_from_guest:
 * @self: #SpiceGtkSession
 *
 * Copy guest clipboard to client-side clipboard.
 *
 * Since 0.8
 **/
void spice_gtk_session_paste_from_guest(SpiceGtkSession *self)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));
    g_return_if_fail(read_only(self) == FALSE);

    SpiceGtkSessionPrivate *s = self->priv;
    int selection = VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD;

    if (s->nclip_targets[selection] == 0) {
        g_warning("Guest clipboard is not available.");
        return;
    }

    {
        GdkContentProvider *provider;
        provider = spice_content_provider_new(self, selection,
            s->clip_target_info[selection],
            s->nclip_targets[selection]);
        if (!gdk_clipboard_set_content(s->clipboard, provider)) {
            g_warning("Clipboard grab failed");
            g_object_unref(provider);
            return;
        }
        g_weak_ref_set(&s->guest_provider[selection], provider);
        g_object_unref(provider);
    }
    s->clipboard_by_guest[selection] = clipboard_is_owned(self, s->clipboard, selection);
    s->clip_hasdata[selection] = !s->clipboard_by_guest[selection];
}

G_GNUC_INTERNAL
void spice_gtk_session_sync_keyboard_modifiers(SpiceGtkSession *self)
{
    GList *l = NULL, *channels = spice_session_get_channels(self->priv->session);

    for (l = channels; l != NULL; l = l->next) {
        if (SPICE_IS_INPUTS_CHANNEL(l->data)) {
            SpiceInputsChannel *inputs = SPICE_INPUTS_CHANNEL(l->data);
            spice_gtk_session_sync_keyboard_modifiers_for_channel(self, inputs, TRUE);
        }
    }
    g_list_free(channels);
}

G_GNUC_INTERNAL
void spice_gtk_session_set_pointer_grabbed(SpiceGtkSession *self, gboolean grabbed)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    self->priv->pointer_grabbed = grabbed;
    g_object_notify(G_OBJECT(self), "pointer-grabbed");
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_pointer_grabbed(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->pointer_grabbed;
}

G_GNUC_INTERNAL
void spice_gtk_session_set_keyboard_has_focus(SpiceGtkSession *self,
                                                gboolean keyboard_has_focus)
{
    g_return_if_fail(SPICE_IS_GTK_SESSION(self));

    self->priv->keyboard_has_focus = keyboard_has_focus;
}

G_GNUC_INTERNAL
void spice_gtk_session_set_mouse_has_pointer(SpiceGtkSession *self,
                                                gboolean mouse_has_pointer)
{
   g_return_if_fail(SPICE_IS_GTK_SESSION(self));
   self->priv->mouse_has_pointer = mouse_has_pointer;
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_keyboard_has_focus(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->keyboard_has_focus;
}

G_GNUC_INTERNAL
gboolean spice_gtk_session_get_mouse_has_pointer(SpiceGtkSession *self)
{
    g_return_val_if_fail(SPICE_IS_GTK_SESSION(self), FALSE);

    return self->priv->mouse_has_pointer;
}
