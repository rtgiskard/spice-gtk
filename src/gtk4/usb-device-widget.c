/*
   Copyright (C) 2012 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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
#include <glib/gi18n-lib.h>
#include "spice-client.h"
#include "spice-marshal.h"
#include "usb-device-widget.h"
#include "usb-device-manager.h"

/**
 * SECTION:usb-device-widget
 * @short_description: USB device selection widget
 * @title: Spice USB device selection widget
 * @section_id:
 * @see_also:
 * @stability: Stable
 * @include: spice-client-gtk.h
 *
 * #SpiceUsbDeviceWidget is a gtk widget which apps can use to easily
 * add an UI to select USB devices to redirect (or unredirect).
 */

struct _SpiceUsbDeviceWidget
{
    GtkBox parent;

    SpiceUsbDeviceWidgetPrivate *priv;
};

struct _SpiceUsbDeviceWidgetClass
{
    GtkBoxClass parent_class;

    /* signals */
    void (*connect_failed) (SpiceUsbDeviceWidget *widget,
                            SpiceUsbDevice *device, GError *error);
};

/* ------------------------------------------------------------------ */
/* Prototypes for callbacks  */
static void device_added_cb(SpiceUsbDeviceManager *manager, SpiceUsbDevice *device,
                            gpointer user_data);
static void device_removed_cb(SpiceUsbDeviceManager *manager, SpiceUsbDevice *device,
                              gpointer user_data);
static void device_error_cb(SpiceUsbDeviceManager *manager, SpiceUsbDevice *device,
                            GError *err, gpointer user_data);
static void empty_cd_clicked_cb(GtkCheckButton *toggle, gpointer user_data);

static gboolean spice_usb_device_widget_update_status(gpointer user_data);

enum {
    PROP_0,
    PROP_SESSION,
    PROP_DEVICE_FORMAT_STRING,
};

enum {
    CONNECT_FAILED,
    LAST_SIGNAL,
};

struct _SpiceUsbDeviceWidgetPrivate {
    SpiceSession *session;
    gchar *device_format_string;
    SpiceUsbDeviceManager *manager;
    GtkWidget *empty_cd;
    GtkWidget *info_bar;
    GtkWidget *label;
    gchar *err_msg;
    gsize device_count;
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE_WITH_PRIVATE(SpiceUsbDeviceWidget, spice_usb_device_widget, GTK_TYPE_BOX)

static void spice_usb_device_widget_get_property(GObject *gobject,
                                                 guint prop_id,
                                                 GValue *value,
                                                 GParamSpec *pspec)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(gobject);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        g_value_set_object(value, priv->session);
        break;
    case PROP_DEVICE_FORMAT_STRING:
        g_value_set_string(value, priv->device_format_string);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_widget_set_property(GObject *gobject,
                                                 guint prop_id,
                                                 const GValue *value,
                                                 GParamSpec *pspec)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(gobject);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    switch (prop_id) {
    case PROP_SESSION:
        priv->session = g_value_dup_object(value);
        break;
    case PROP_DEVICE_FORMAT_STRING:
        priv->device_format_string = g_value_dup_string(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(gobject, prop_id, pspec);
        break;
    }
}

static void spice_usb_device_widget_add_err_msg(SpiceUsbDeviceWidget *self,
                                                gchar *new_err_msg)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    if (priv->err_msg) {
        /* Append the new error message to err_msg,
           but only if it's *not* already there! */
        if (!strstr(priv->err_msg, new_err_msg)) {
            gchar *old_err_msg = priv->err_msg;
            priv->err_msg = g_strdup_printf("%s\n%s", old_err_msg, new_err_msg);
            g_free(old_err_msg);
        }
        g_free(new_err_msg);
    } else {
        priv->err_msg = new_err_msg;
    }
}

static void spice_usb_device_widget_hide_info_bar(SpiceUsbDeviceWidget *self)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    if (priv->info_bar) {
        gtk_widget_unparent(priv->info_bar);
        priv->info_bar = NULL;
    }
}

/*
 * GtkInfoBar replacement — GtkInfoBar was deprecated in GTK 4.10.
 * We build an equivalent styled box: an outer frame with a CSS class
 * ("warning" or "info") containing an icon + label.
 */
static void
spice_usb_device_widget_show_info_bar(SpiceUsbDeviceWidget *self,
                                      const gchar *message,
                                      const gchar *css_class,
                                      const gchar *icon_name)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *frame, *hbox, *widget;

    spice_usb_device_widget_hide_info_bar(self);

    frame = gtk_frame_new(NULL);
    gtk_widget_add_css_class(frame, css_class);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(hbox, 8);
    gtk_widget_set_margin_end(hbox, 8);
    gtk_widget_set_margin_top(hbox, 6);
    gtk_widget_set_margin_bottom(hbox, 6);
    gtk_frame_set_child(GTK_FRAME(frame), hbox);

    widget = gtk_image_new_from_icon_name(icon_name);
    gtk_box_append(GTK_BOX(hbox), widget);

    widget = gtk_label_new(message);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    gtk_box_append(GTK_BOX(hbox), widget);

    priv->info_bar = frame;
    gtk_widget_set_margin_start(frame, 12);
    gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(self), priv->info_bar);
}

static void
empty_cd_file_dialog_cb(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &err);

    if (file != NULL) {
        gchar *filename = g_file_get_path(file);

        if (filename != NULL) {
            if (!spice_usb_device_manager_create_shared_cd_device(priv->manager, filename, &err) && err) {
                gchar *basename = g_path_get_basename(filename);
                gchar *err_msg = g_strdup_printf(_("shared CD %s, %s"), basename, err->message);
                g_free(basename);
                spice_usb_device_widget_add_err_msg(self, err_msg);
                spice_usb_device_widget_update_status(self);
            }
            g_free(filename);
        }
        g_object_unref(file);
    }
    g_clear_error(&err);
    g_object_unref(self);
}

static void
empty_cd_clicked_cb(GtkCheckButton *toggle, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);

    if (!gtk_check_button_get_active(toggle))
        return;
    gtk_check_button_set_active(toggle, FALSE);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Select ISO file or device"));
    gtk_file_dialog_open(dialog,
                         GTK_WINDOW(gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW)),
                         NULL,
                         empty_cd_file_dialog_cb,
                         g_object_ref(self));
    g_object_unref(dialog);
}

static void spice_usb_device_widget_add_empty_cd(SpiceUsbDeviceWidget *self)
{
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *empty_cd, *cd_label;

    empty_cd = gtk_check_button_new();
    cd_label = gtk_label_new(_("SPICE CD (empty)"));
    gtk_label_set_ellipsize(GTK_LABEL(cd_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_check_button_set_child(GTK_CHECK_BUTTON(empty_cd), cd_label);
    g_signal_connect(G_OBJECT(empty_cd), "toggled", G_CALLBACK(empty_cd_clicked_cb), self);

    gtk_widget_set_margin_start(empty_cd, 12);
    gtk_box_append(GTK_BOX(self), empty_cd);

    priv->empty_cd = empty_cd;
}

static void spice_usb_device_widget_constructed(GObject *gobject)
{
    SpiceUsbDeviceWidget *self;
    SpiceUsbDeviceWidgetPrivate *priv;
    GPtrArray *devices = NULL;
    GError *err = NULL;
    gchar *str;

    self = SPICE_USB_DEVICE_WIDGET(gobject);
    priv = self->priv;
    if (!priv->session) {
        g_error("SpiceUsbDeviceWidget constructed without a session");
    }

    priv->label = gtk_label_new(NULL);
    str = g_strdup_printf("<b>%s</b>", _("Select USB devices to redirect"));
    gtk_label_set_markup(GTK_LABEL (priv->label), str);
    g_free(str);
    gtk_label_set_xalign(GTK_LABEL(priv->label), 0.0);
    gtk_label_set_yalign(GTK_LABEL(priv->label), 0.5);
    gtk_box_append(GTK_BOX(self), priv->label);

    priv->manager = spice_usb_device_manager_get(priv->session, &err);
    if (err) {
        spice_usb_device_widget_show_info_bar(self, err->message,
                                               "warning",
                                               "dialog-warning");
        g_clear_error(&err);
        return;
    }

    g_signal_connect(priv->manager, "device-added",
                     G_CALLBACK(device_added_cb), self);
    g_signal_connect(priv->manager, "device-removed",
                     G_CALLBACK(device_removed_cb), self);
    g_signal_connect(priv->manager, "device-error",
                     G_CALLBACK(device_error_cb), self);

    spice_usb_device_widget_add_empty_cd(self);

    devices = spice_usb_device_manager_get_devices(priv->manager);
    if (devices != NULL) {
        int i;
        for (i = 0; i < devices->len; i++) {
            device_added_cb(NULL, g_ptr_array_index(devices, i), self);
        }

        g_ptr_array_unref(devices);
    }

    spice_usb_device_widget_update_status(self);
}

static void spice_usb_device_widget_finalize(GObject *object)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(object);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;

    if (priv->manager) {
        g_signal_handlers_disconnect_by_func(priv->manager, device_added_cb, self);
        g_signal_handlers_disconnect_by_func(priv->manager, device_removed_cb, self);
        g_signal_handlers_disconnect_by_func(priv->manager, device_error_cb, self);
    }
    g_clear_object(&priv->session);
    g_clear_pointer(&priv->device_format_string, g_free);
    g_clear_pointer(&priv->err_msg, g_free);

    G_OBJECT_CLASS(spice_usb_device_widget_parent_class)->finalize(object);
}

static void spice_usb_device_widget_class_init(SpiceUsbDeviceWidgetClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;
    GParamSpec *pspec;

    gobject_class->constructed  = spice_usb_device_widget_constructed;
    gobject_class->finalize     = spice_usb_device_widget_finalize;
    gobject_class->get_property = spice_usb_device_widget_get_property;
    gobject_class->set_property = spice_usb_device_widget_set_property;

    /**
     * SpiceUsbDeviceWidget:session:
     *
     * #SpiceSession this #SpiceUsbDeviceWidget is associated with
     *
     **/
    pspec = g_param_spec_object("session",
                                "Session",
                                "SpiceSession",
                                SPICE_TYPE_SESSION,
                                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_SESSION, pspec);

    /**
     * SpiceUsbDeviceWidget:device-format-string:
     *
     * Format string to pass to spice_usb_device_get_description() for getting
     * the device USB descriptions.
     */
    pspec = g_param_spec_string("device-format-string",
                                "Device format string",
                                "Format string for device description",
                                NULL,
                                G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
                                G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(gobject_class, PROP_DEVICE_FORMAT_STRING,
                                    pspec);

    /**
     * SpiceUsbDeviceWidget::connect-failed:
     * @widget: The #SpiceUsbDeviceWidget that emitted the signal
     * @device: #SpiceUsbDevice boxed object corresponding to the added device
     * @error:  #GError describing the reason why the connect failed
     *
     * The #SpiceUsbDeviceWidget::connect-failed signal is emitted whenever
     * the user has requested for a device to be redirected and this has
     * failed.
     **/
    signals[CONNECT_FAILED] =
        g_signal_new("connect-failed",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceUsbDeviceWidgetClass, connect_failed),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__BOXED_BOXED,
                     G_TYPE_NONE,
                     2,
                     SPICE_TYPE_USB_DEVICE,
                     G_TYPE_ERROR);
}

static void spice_usb_device_widget_init(SpiceUsbDeviceWidget *self)
{
    self->priv = spice_usb_device_widget_get_instance_private(self);
}

/* ------------------------------------------------------------------ */
/* public api                                                         */

/**
 * spice_usb_device_widget_new:
 * @session: #SpiceSession for which to widget will control USB redirection
 * @device_format_string: (allow-none): String passed to
 * spice_usb_device_get_description()
 *
 * Creates a new widget to control USB redirection.
 *
 * Returns: a new #SpiceUsbDeviceWidget instance
 */
GtkWidget *spice_usb_device_widget_new(SpiceSession *session,
                                       const gchar *device_format_string)
{
    return g_object_new(SPICE_TYPE_USB_DEVICE_WIDGET,
                        "orientation", GTK_ORIENTATION_VERTICAL,
                        "session", session,
                        "device-format-string", device_format_string,
                        "spacing", 6,
                        NULL);
}

/* ------------------------------------------------------------------ */
/* callbacks                                                          */

static SpiceUsbDevice *get_usb_device(GtkWidget *widget)
{
    return g_object_get_data(G_OBJECT(widget), "usb-device");
}

static void check_can_redirect(GtkWidget *widget, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *device;
    gboolean can_redirect;
    GError *err = NULL;

    device = get_usb_device(widget);
    if (!device) {
        return; /* Non device widget, ie the info_bar */
    }

    priv->device_count++;

    if (spice_usb_device_manager_is_redirecting(priv->manager)) {
        can_redirect = FALSE;
    } else {
        can_redirect = spice_usb_device_manager_can_redirect_device(priv->manager,
                                                                    device, &err);

        if (!can_redirect) {
            gchar *err_msg = g_strdup_printf(_("Can't redirect: %s"), err->message);
            spice_usb_device_widget_add_err_msg(self, err_msg);
        }
        g_clear_error(&err);
    }
    gtk_widget_set_sensitive(widget, can_redirect);
}

static void container_foreach(GtkWidget *container,
                              void (*callback)(GtkWidget *widget, gpointer data),
                              gpointer callback_data)
{
    GList *children = NULL;
    GtkWidget *child;

    /* Collect children first so callback can safely modify the tree */
    for (child = gtk_widget_get_first_child(container);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        children = g_list_prepend(children, g_object_ref(child));
    }
    children = g_list_reverse(children);

    for (GList *l = children; l != NULL; l = l->next) {
        callback(GTK_WIDGET(l->data), callback_data);
    }
    g_list_free_full(children, g_object_unref);
}

static gboolean spice_usb_device_widget_update_status(gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    gchar *str, *markup_str;
    const gchar *free_channels_str;
    int free_channels;
    gboolean redirecting;

    redirecting = spice_usb_device_manager_is_redirecting(priv->manager);

    g_object_get(priv->manager, "free-channels", &free_channels, NULL);
    free_channels_str = g_dngettext(GETTEXT_PACKAGE,
                                    "Select USB devices to redirect (%d free channel)",
                                    "Select USB devices to redirect (%d free channels)",
                                    free_channels);
    str = g_strdup_printf(free_channels_str, free_channels);
    markup_str = g_strdup_printf("<b>%s</b>", str);
    gtk_label_set_markup(GTK_LABEL (priv->label), markup_str);
    g_free(markup_str);
    g_free(str);

    priv->device_count = 0;
    container_foreach(GTK_WIDGET(self), check_can_redirect, self);

    if (priv->err_msg) {
        spice_usb_device_widget_show_info_bar(self, priv->err_msg,
                                               "info",
                                               "dialog-warning");
        g_clear_pointer(&priv->err_msg, g_free);
    } else if (redirecting) {
        spice_usb_device_widget_show_info_bar(self, _("Redirecting USB Device..."),
                                               "info",
                                               "dialog-information");
    } else {
        spice_usb_device_widget_hide_info_bar(self);
    }

    if (priv->device_count == 0) {
        spice_usb_device_widget_show_info_bar(self, _("No USB devices detected"),
                                               "info",
                                               "dialog-information");
    }
    return FALSE;
}

typedef struct _connect_cb_data {
    GtkCheckButton *check;
    SpiceUsbDeviceWidget *self;
} connect_cb_data;

static void connect_cb_data_free(connect_cb_data *data)
{
    spice_usb_device_widget_update_status(data->self);
    g_object_unref(data->check);
    g_object_unref(data->self);
    g_free(data);
}

static void _disconnect_cb(GObject *gobject, GAsyncResult *res, gpointer user_data)
{
    SpiceUsbDeviceManager *manager = SPICE_USB_DEVICE_MANAGER(gobject);
    connect_cb_data *data = user_data;
    GError *err = NULL;

    spice_usb_device_manager_disconnect_device_finish(manager, res, &err);
    if (err) {
        SPICE_DEBUG("Device disconnection failed");
        g_error_free(err);
    }

    connect_cb_data_free(data);
}

static void checkbox_clicked_cb(GtkCheckButton *check, gpointer user_data);
static void connect_cb(GObject *gobject, GAsyncResult *res, gpointer user_data)
{
    SpiceUsbDeviceManager *manager = SPICE_USB_DEVICE_MANAGER(gobject);
    connect_cb_data *data = user_data;
    SpiceUsbDeviceWidget *self = data->self;
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *device;
    GError *err = NULL;
    gchar *desc;

    spice_usb_device_manager_connect_device_finish(manager, res, &err);
    if (err) {
        device = g_object_get_data(G_OBJECT(data->check), "usb-device");
        desc = spice_usb_device_get_description(device,
                                                priv->device_format_string);
        g_prefix_error(&err, "Could not redirect %s: ", desc);
        g_free(desc);

        SPICE_DEBUG("%s", err->message);
        g_signal_emit(self, signals[CONNECT_FAILED], 0, device, err);
        g_error_free(err);

        /* don't trigger a disconnect if connect failed */
        g_signal_handlers_block_by_func(data->check, checkbox_clicked_cb, self);
        gtk_check_button_set_active(data->check, FALSE);
        g_signal_handlers_unblock_by_func(data->check, checkbox_clicked_cb, self);
    }

    connect_cb_data_free(data);
}

static void checkbox_clicked_cb(GtkCheckButton *check, gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    SpiceUsbDevice *device;

    device = g_object_get_data(G_OBJECT(check), "usb-device");
    connect_cb_data *data = g_new(connect_cb_data, 1);
    data->check = g_object_ref(check);
    data->self  = g_object_ref(self);

    if (gtk_check_button_get_active(check)) {
        spice_usb_device_manager_connect_device_async(priv->manager,
                                                      device,
                                                      NULL,
                                                      connect_cb,
                                                      data);
    } else {
        spice_usb_device_manager_disconnect_device_async(priv->manager,
                                                         device,
                                                         NULL,
                                                         _disconnect_cb,
                                                         data);

    }
    spice_usb_device_widget_update_status(self);
}

static void checkbox_usb_device_destroy_notify(gpointer data)
{
    g_boxed_free(spice_usb_device_get_type(), data);
}

static void device_added_cb(SpiceUsbDeviceManager *manager,
                            SpiceUsbDevice *device,
                            gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);
    SpiceUsbDeviceWidgetPrivate *priv = self->priv;
    GtkWidget *check;
    gchar *desc;

    desc = spice_usb_device_get_description(device,
                                            priv->device_format_string);
    check = gtk_check_button_new_with_label(desc);
    g_free(desc);

    if (spice_usb_device_manager_is_device_connected(priv->manager, device))
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);

    g_object_set_data_full(G_OBJECT(check), "usb-device",
                           g_boxed_copy(spice_usb_device_get_type(), device),
                           checkbox_usb_device_destroy_notify);
    g_signal_connect(check, "toggled", G_CALLBACK(checkbox_clicked_cb), self);

    gtk_widget_set_margin_start(check, 12);
    gtk_box_append(GTK_BOX(self), check);

    /* Keep the empty CD entry after device rows. */
    gtk_box_reorder_child_after(GTK_BOX(self), priv->empty_cd, check);

    if (spice_usb_device_manager_is_device_shared_cd(priv->manager, device) &&
        !gtk_check_button_get_active(GTK_CHECK_BUTTON(check))) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
    }

    spice_usb_device_widget_update_status(self);
}

static void destroy_widget_by_usb_device(GtkWidget *widget, gpointer user_data)
{
    if (get_usb_device(widget) == user_data) {
        gtk_widget_unparent(widget);
    }
}

static void device_removed_cb(SpiceUsbDeviceManager *manager,
                              SpiceUsbDevice *device,
                              gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);

    container_foreach(GTK_WIDGET(self),
                      destroy_widget_by_usb_device, device);

    spice_usb_device_widget_update_status(self);
}

static void set_inactive_by_usb_device(GtkWidget *check, gpointer user_data)
{
    if (get_usb_device(check) == user_data) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), FALSE);
    }
}

static void device_error_cb(SpiceUsbDeviceManager *manager,
                            SpiceUsbDevice *device,
                            GError *err,
                            gpointer user_data)
{
    SpiceUsbDeviceWidget *self = SPICE_USB_DEVICE_WIDGET(user_data);

    container_foreach(GTK_WIDGET(self),
                      set_inactive_by_usb_device, device);

    spice_usb_device_widget_update_status(self);
}
