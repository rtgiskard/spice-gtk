/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#include <sys/stat.h>
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#include "spice-widget.h"
#include "spice-gtk-session.h"
#include "spice-audio.h"
#include "spice-common.h"
#include "spice-cmdline.h"
#include "spice-option.h"
#include "usb-device-widget.h"

#include "spicy-connect.h"
#include <gst/gst.h>

typedef struct spice_connection spice_connection;

enum {
    STATE_SCROLL_LOCK,
    STATE_CAPS_LOCK,
    STATE_NUM_LOCK,
    STATE_MAX,
};

#define SPICE_TYPE_WINDOW                  (spice_window_get_type ())
#define SPICE_WINDOW(obj)                  (G_TYPE_CHECK_INSTANCE_CAST ((obj), SPICE_TYPE_WINDOW, SpiceWindow))
#define SPICE_IS_WINDOW(obj)               (G_TYPE_CHECK_INSTANCE_TYPE ((obj), SPICE_TYPE_WINDOW))
#define SPICE_WINDOW_CLASS(klass)          (G_TYPE_CHECK_CLASS_CAST ((klass), SPICE_TYPE_WINDOW, SpiceWindowClass))
#define SPICE_IS_WINDOW_CLASS(klass)       (G_TYPE_CHECK_CLASS_TYPE ((klass), SPICE_TYPE_WINDOW))
#define SPICE_WINDOW_GET_CLASS(obj)        (G_TYPE_INSTANCE_GET_CLASS ((obj), SPICE_TYPE_WINDOW, SpiceWindowClass))

typedef struct _SpiceWindow SpiceWindow;
typedef struct _SpiceWindowClass SpiceWindowClass;

struct _SpiceWindow {
    GObject            object;
    spice_connection   *conn;
    gint               id;
    gint               monitor_id;
    GtkWidget          *toplevel, *spice;
    GtkWidget          *menubar, *toolbar;
    GtkWidget          *ritem, *rmenu;
    GtkWidget          *statusbar, *status, *st[STATE_MAX];
    GSimpleActionGroup *ag;
    GtkBuilder         *ui;
    bool               fullscreen;
    bool               mouse_grabbed;
    SpiceChannel       *display_channel;
#ifdef G_OS_WIN32
    gint               win_x;
    gint               win_y;
#endif
    gboolean           enable_accels_save;
    gboolean           enable_mnemonics_save;
};

struct _SpiceWindowClass
{
  GObjectClass parent_class;
};

static GType spice_window_get_type(void);

G_DEFINE_TYPE (SpiceWindow, spice_window, G_TYPE_OBJECT)

#define CHANNELID_MAX 4
#define MONITORID_MAX 4


// FIXME: turn this into an object, get rid of fixed wins array, use
// signals to replace the various callback that iterate over wins array
struct spice_connection {
    SpiceSession     *session;
    SpiceGtkSession  *gtk_session;
    SpiceMainChannel *main;
    SpiceWindow     *wins[CHANNELID_MAX * MONITORID_MAX];
    SpiceAudio       *audio;
    const char       *mouse_state;
    const char       *agent_state;
    gboolean         agent_connected;
    int              disconnecting;

    /* key: SpiceFileTransferTask, value: TransferTaskWidgets */
    GHashTable *transfers;
    GtkWidget *transfer_dialog;
};

static gboolean is_gtk_session_property(const gchar *property);
static void recent_item_activated_cb(GSimpleAction *chooser,
                                     GVariant *variant,
                                     gpointer data);
static void compression_cb(GSimpleAction *action,
                           GVariant *variant,
                           gpointer user_data);
static void video_codec_type_cb(GSimpleAction *action,
                                GVariant *variant,
                                gpointer user_data);
static void del_window(spice_connection *conn, SpiceWindow *win);
static spice_connection *connection_new(void);
static void connection_connect(spice_connection *conn);
static void connection_disconnect(spice_connection *conn);
static void connection_destroy(SpiceSession *session,
                               spice_connection *conn);
static void usb_connect_failed(GObject               *object,
                               SpiceUsbDevice        *device,
                               GError                *error,
                               gpointer               data);

/* options */
static gboolean fullscreen = false;
static gboolean hide_menu_bar = false;
static gboolean version = false;
static char *spicy_title = NULL;
/* globals */
static GMainLoop     *mainloop = NULL;
static int           connections = 0;
static GKeyFile      *keyfile = NULL;
static SpicePortChannel*stdin_port = NULL;

/* ------------------------------------------------------------------ */

static int ask_user(GtkWidget *parent, char *title, char *message,
                    char *dest, int dlen, int hide)
{
    GtkWidget *dialog, *area, *label, *entry;
    const char *txt;
    int retval;

    /* Create the widgets */
    dialog = gtk_dialog_new_with_buttons(title,
                                         parent ? GTK_WINDOW(parent) : NULL,
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_OK",
                                         GTK_RESPONSE_ACCEPT,
                                         "_Cancel",
                                         GTK_RESPONSE_REJECT,
                                         NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    label = gtk_label_new(message);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 5);

    entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), dest);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (hide)
        gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 5);

    /* show and wait for response */
    gtk_widget_show_all(dialog);
    switch (gtk_dialog_run(GTK_DIALOG(dialog))) {
    case GTK_RESPONSE_ACCEPT:
        txt = gtk_entry_get_text(GTK_ENTRY(entry));
        snprintf(dest, dlen, "%s", txt);
        retval = 0;
        break;
    default:
        retval = -1;
        break;
    }
    gtk_widget_destroy(dialog);
    return retval;
}

static void update_status_window(SpiceWindow *win)
{
    GString *status;

    if (win == NULL)
        return;

    status = g_string_new(NULL);
    g_string_printf(status, "mouse: %6s, agent: %3s",
                    win->conn->mouse_state,
                    win->conn->agent_state);

    if (win->mouse_grabbed) {
        SpiceGrabSequence *sequence = spice_display_get_grab_keys(SPICE_DISPLAY(win->spice));
        gchar *seq = spice_grab_sequence_as_string(sequence);
        g_string_append_printf(status, "\tUse %s to ungrab mouse", seq);
        g_free(seq);
    }

    gtk_label_set_text(GTK_LABEL(win->status), status->str);
    g_string_free(status, TRUE);
}

static void update_status(struct spice_connection *conn)
{
    int i;

    for (i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        if (conn->wins[i] == NULL)
            continue;
        update_status_window(conn->wins[i]);
    }
}

static const char *spice_edit_properties[] = {
    "CopyToGuest",
    "PasteFromGuest",
};

static void update_edit_menu_window(SpiceWindow *win)
{
    int i;
    GAction *toggle;

    if (win == NULL) {
        return;
    }

    /* Make "CopyToGuest" and "PasteFromGuest" insensitive if spice
     * agent is not connected */
    for (i = 0; i < G_N_ELEMENTS(spice_edit_properties); i++) {
        toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), spice_edit_properties[i]);
        if (G_IS_SIMPLE_ACTION(toggle)) {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(toggle), win->conn->agent_connected);
        }
    }
}

static void update_edit_menu(struct spice_connection *conn)
{
    int i;

    for (i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        if (conn->wins[i]) {
            update_edit_menu_window(conn->wins[i]);
        }
    }
}

static void menu_cb_connect(GSimpleAction *action, GVariant *variant, gpointer data)
{
    struct spice_connection *conn;

    conn = connection_new();
    connection_connect(conn);
}

static void menu_cb_close(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;

    connection_disconnect(win->conn);
}

static void menu_cb_copy(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;

    spice_gtk_session_copy_to_guest(win->conn->gtk_session);
}

static void menu_cb_paste(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;

    spice_gtk_session_paste_from_guest(win->conn->gtk_session);
}

static void window_set_fullscreen(SpiceWindow *win, gboolean fs)
{
    if (fs) {
#ifdef G_OS_WIN32
        gtk_window_get_position(GTK_WINDOW(win->toplevel), &win->win_x, &win->win_y);
#endif
        gtk_window_fullscreen(GTK_WINDOW(win->toplevel));
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(win->toplevel));
#ifdef G_OS_WIN32
        gtk_window_move(GTK_WINDOW(win->toplevel), win->win_x, win->win_y);
#endif
    }
}

static void menu_cb_fullscreen(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;

    window_set_fullscreen(win, !win->fullscreen);
}

#ifdef USE_SMARTCARD
static void enable_smartcard_actions(SpiceWindow *win, VReader *reader,
                                     gboolean can_insert, gboolean can_remove)
{
    GAction *action;

    if ((reader != NULL) && (!spice_smartcard_reader_is_software((SpiceSmartcardReader*)reader)))
    {
        /* Having menu actions to insert/remove smartcards only makes sense
         * for software smartcard readers, don't do anything when the event
         * we received was for a "real" smartcard reader.
         */
        return;
    }
    action = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "InsertSmartcard");
    g_return_if_fail(action != NULL);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_insert);

    action = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "RemoveSmartcard");
    g_return_if_fail(action != NULL);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), can_remove);
}


static void reader_added_cb(SpiceSmartcardManager *manager, VReader *reader,
                            gpointer user_data)
{
    enable_smartcard_actions(user_data, reader, TRUE, FALSE);
}

static void reader_removed_cb(SpiceSmartcardManager *manager, VReader *reader,
                              gpointer user_data)
{
    enable_smartcard_actions(user_data, reader, FALSE, FALSE);
}

static void card_inserted_cb(SpiceSmartcardManager *manager, VReader *reader,
                             gpointer user_data)
{
    enable_smartcard_actions(user_data, reader, FALSE, TRUE);
}

static void card_removed_cb(SpiceSmartcardManager *manager, VReader *reader,
                            gpointer user_data)
{
    enable_smartcard_actions(user_data, reader, TRUE, FALSE);
}

static void menu_cb_insert_smartcard(GSimpleAction *action, GVariant *variant, gpointer data)
{
    spice_smartcard_manager_insert_card(spice_smartcard_manager_get());
}

static void menu_cb_remove_smartcard(GSimpleAction *action, GVariant *variant, gpointer data)
{
    spice_smartcard_manager_remove_card(spice_smartcard_manager_get());
}
#endif

static void menu_cb_mouse_mode(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;
    SpiceMainChannel *cmain = win->conn->main;
    int mode;

    g_object_get(cmain, "mouse-mode", &mode, NULL);
    if (mode == SPICE_MOUSE_MODE_CLIENT)
        mode = SPICE_MOUSE_MODE_SERVER;
    else
        mode = SPICE_MOUSE_MODE_CLIENT;

    spice_main_channel_request_mouse_mode(cmain, mode);
}

#ifdef USE_USBREDIR
static void remove_cb(GtkContainer *container, GtkWidget *widget, void *data)
{
    gtk_window_resize(GTK_WINDOW(data), 1, 1);
}

static void menu_cb_select_usb_devices(GSimpleAction *action, GVariant *variant, gpointer data)
{
    GtkWidget *dialog, *area, *usb_device_widget;
    SpiceWindow *win = data;

    /* Create the widgets */
    dialog = gtk_dialog_new_with_buttons(
                    "Select USB devices for redirection",
                    GTK_WINDOW(win->toplevel),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    "_Close", GTK_RESPONSE_ACCEPT,
                    NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_container_set_border_width(GTK_CONTAINER(dialog), 12);
    gtk_box_set_spacing(GTK_BOX(gtk_bin_get_child(GTK_BIN(dialog))), 12);

    area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    usb_device_widget = spice_usb_device_widget_new(win->conn->session,
                                                    NULL); /* default format */
    g_signal_connect(usb_device_widget, "connect-failed",
                     G_CALLBACK(usb_connect_failed), NULL);
    gtk_box_pack_start(GTK_BOX(area), usb_device_widget, TRUE, TRUE, 0);

    /* This shrinks the dialog when USB devices are unplugged */
    g_signal_connect(usb_device_widget, "remove",
                     G_CALLBACK(remove_cb), dialog);

    /* show and run */
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}
#endif

static void menu_cb_bool_prop(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;
    gboolean state = g_variant_get_boolean(variant);
    const char *name;
    gpointer object;

    name = g_action_get_name(G_ACTION(action));
    SPICE_DEBUG("%s: %s = %s", __FUNCTION__, name, state ? "yes" : "no");

    g_key_file_set_boolean(keyfile, "general", name, state);

    if (is_gtk_session_property(name)) {
        object = win->conn->gtk_session;
    } else {
        object = win->spice;
    }
    g_object_set(object, name, state, NULL);
    g_simple_action_set_state(action, variant);
}

static void menu_cb_conn_bool_prop_changed(GObject    *gobject,
                                           GParamSpec *pspec,
                                           gpointer    user_data)
{
    SpiceWindow *win = user_data;
    const gchar *property = g_param_spec_get_name(pspec);
    GAction *toggle;
    gboolean state;

    toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), property);
    g_object_get(win->conn->gtk_session, property, &state, NULL);
    g_simple_action_set_state(G_SIMPLE_ACTION(toggle), g_variant_new_boolean(state));
}

static void menu_cb_toolbar(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;
    gboolean state = g_variant_get_boolean(variant);

    gtk_widget_set_visible(win->toolbar, state);
    g_key_file_set_boolean(keyfile, "ui", "toolbar", state);
    g_simple_action_set_state(action, variant);
}

static void menu_cb_statusbar(GSimpleAction *action, GVariant *variant, gpointer data)
{
    SpiceWindow *win = data;
    gboolean state = g_variant_get_boolean(variant);

    gtk_widget_set_visible(win->statusbar, state);
    g_key_file_set_boolean(keyfile, "ui", "statusbar", state);
    g_simple_action_set_state(action, variant);
}

static void menu_cb_about(GSimpleAction *action, GVariant *variant, gpointer data)
{
    char *comments = "gtk test client app for the\n"
        "spice remote desktop protocol";
    static const char *copyright = "(c) 2010 Red Hat";
    static const char *website = "http://www.spice-space.org";
    static const char *authors[] = { "Gerd Hoffmann <kraxel@redhat.com>",
                               "Marc-André Lureau <marcandre.lureau@redhat.com>",
                               NULL };
    SpiceWindow *win = data;

    gtk_show_about_dialog(GTK_WINDOW(win->toplevel),
                          "authors",         authors,
                          "comments",        comments,
                          "copyright",       copyright,
                          "logo-icon-name",  "help-about",
                          "website",         website,
                          "version",         PACKAGE_VERSION,
                          "license",         "LGPLv2.1",
                          NULL);
}

static gboolean delete_cb(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    SpiceWindow *win = data;

    if (win->monitor_id == 0)
        connection_disconnect(win->conn);
    else
        del_window(win->conn, win);

    return true;
}

static gboolean window_state_cb(GtkWidget *widget, GdkEventWindowState *event,
                                gpointer data)
{
    SpiceWindow *win = data;
    if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
        win->fullscreen = event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN;
        if (win->fullscreen) {
            gtk_widget_hide(win->menubar);
            gtk_widget_hide(win->toolbar);
            gtk_widget_hide(win->statusbar);
            gtk_widget_grab_focus(win->spice);
        } else {
            gboolean state;
            GAction *toggle;
            GVariant *variant;
            if (!hide_menu_bar) {
                gtk_widget_show(win->menubar);
            }
            toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "Toolbar");
            variant = g_action_get_state(toggle);
            state = g_variant_get_boolean(variant);
            g_variant_unref(variant);
            gtk_widget_set_visible(win->toolbar, state);
            toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "Statusbar");
            variant = g_action_get_state(toggle);
            state = g_variant_get_boolean(variant);
            g_variant_unref(variant);
            gtk_widget_set_visible(win->statusbar, state);
        }
    }
    return TRUE;
}

static void grab_keys_pressed_cb(GtkWidget *widget, gpointer data)
{
    SpiceWindow *win = data;

    /* since mnemonics are disabled, we leave fullscreen when
       ungrabbing mouse. Perhaps we should have a different handling
       of fullscreen key, or simply use a UI, like vinagre */
    window_set_fullscreen(win, FALSE);
}

static void mouse_grab_cb(GtkWidget *widget, gint grabbed, gpointer data)
{
    SpiceWindow *win = data;

    win->mouse_grabbed = grabbed;
    update_status(win->conn);
}

static void keyboard_grab_cb(GtkWidget *widget, gint grabbed, gpointer data)
{
    SpiceWindow *win = data;
    GtkSettings *settings = gtk_widget_get_settings (widget);

    if (grabbed) {
        /* disable mnemonics & accels */
        g_object_get(settings,
                     "gtk-enable-accels", &win->enable_accels_save,
                     "gtk-enable-mnemonics", &win->enable_mnemonics_save,
                     NULL);
        g_object_set(settings,
                     "gtk-enable-accels", FALSE,
                     "gtk-enable-mnemonics", FALSE,
                     NULL);
    } else {
        g_object_set(settings,
                     "gtk-enable-accels", win->enable_accels_save,
                     "gtk-enable-mnemonics", win->enable_mnemonics_save,
                     NULL);
    }
}

static void menu_cb_resize_to(GSimpleAction *action, GVariant *variant,
                              gpointer data)
{
    SpiceWindow *win = data;
    GtkWidget *dialog;
    GtkWidget *spin_width, *spin_height, *spin_x, *spin_y;
    GtkGrid *grid;
    gint width, height;
    dialog = gtk_dialog_new_with_buttons("Resize guest to",
                                         GTK_WINDOW(win->toplevel),
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Apply",
                                         GTK_RESPONSE_APPLY,
                                         "_Cancel",
                                         GTK_RESPONSE_CANCEL,
                                         NULL);

    spin_width = gtk_spin_button_new_with_range(0, G_MAXINT, 10);
    spin_height = gtk_spin_button_new_with_range(0, G_MAXINT, 10);
    spin_x = gtk_spin_button_new_with_range(0, G_MAXINT, 10);
    spin_y = gtk_spin_button_new_with_range(0, G_MAXINT, 10);

    gtk_widget_get_preferred_width(win->spice, NULL, &width);
    gtk_widget_get_preferred_height(win->spice, NULL, &height);
    width *= gtk_widget_get_scale_factor(win->spice);
    height *= gtk_widget_get_scale_factor(win->spice);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_width), width);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_height), height);

    grid = GTK_GRID(gtk_grid_new());
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
                      GTK_WIDGET(grid));
    gtk_grid_attach(grid, gtk_label_new("Resize the guest display:"), 0, 0, 2, 1);
    gtk_grid_attach(grid, gtk_label_new("width:"), 0, 2, 1, 1);
    gtk_grid_attach(grid, spin_width, 1, 2, 1, 1);
    gtk_grid_attach(grid, gtk_label_new("height:"), 0, 3, 1, 1);
    gtk_grid_attach(grid, spin_height, 1, 3, 1, 1);
    gtk_grid_attach(grid, gtk_label_new("x:"), 0, 4, 1, 1);
    gtk_grid_attach(grid, spin_x, 1, 4, 1, 1);
    gtk_grid_attach(grid, gtk_label_new("y:"), 0, 5, 1, 1);
    gtk_grid_attach(grid, spin_y, 1, 5, 1, 1);

    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG (dialog)) == GTK_RESPONSE_APPLY) {
        spice_main_channel_update_display_enabled(win->conn->main, win->id + win->monitor_id, TRUE,
                                                  FALSE);
        spice_main_channel_update_display(
            win->conn->main,
            win->id + win->monitor_id,
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_x)),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_y)),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_width)),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_height)),
            TRUE);
        spice_main_channel_send_monitor_config(win->conn->main);
    }
    gtk_widget_destroy(dialog);
}

static void restore_configuration(SpiceWindow *win)
{
    gboolean state;
    gchar *str;
    gchar **keys = NULL;
    gsize nkeys, i;
    GError *error = NULL;
    gpointer object;

    keys = g_key_file_get_keys(keyfile, "general", &nkeys, &error);
    if (error != NULL) {
        if (error->code != G_KEY_FILE_ERROR_GROUP_NOT_FOUND)
            g_warning("Failed to read configuration file keys: %s", error->message);
        g_clear_error(&error);
        return;
    }

    if (nkeys > 0)
        g_return_if_fail(keys != NULL);

    for (i = 0; i < nkeys; ++i) {
        if (g_str_equal(keys[i], "grab-sequence"))
            continue;
        state = g_key_file_get_boolean(keyfile, "general", keys[i], &error);
        if (error != NULL) {
            g_clear_error(&error);
            continue;
        }

        if (is_gtk_session_property(keys[i])) {
            object = win->conn->gtk_session;
        } else {
            object = win->spice;
        }
        g_object_set(object, keys[i], state, NULL);
    }

    g_strfreev(keys);

    str = g_key_file_get_string(keyfile, "general", "grab-sequence", &error);
    if (error == NULL) {
        SpiceGrabSequence *seq = spice_grab_sequence_new_from_string(str);
        spice_display_set_grab_keys(SPICE_DISPLAY(win->spice), seq);
        spice_grab_sequence_free(seq);
        g_free(str);
    }
    g_clear_error(&error);


    state = g_key_file_get_boolean(keyfile, "ui", "toolbar", &error);
    if (error == NULL)
        gtk_widget_set_visible(win->toolbar, state);
    g_clear_error(&error);

    state = g_key_file_get_boolean(keyfile, "ui", "statusbar", &error);
    if (error == NULL)
        gtk_widget_set_visible(win->statusbar, state);
    g_clear_error(&error);
}

/* ------------------------------------------------------------------ */

static const GActionEntry entries[] = {
    { "Connect", menu_cb_connect },
    { "Close", menu_cb_close },
    { "CopyToGuest", menu_cb_copy },
    { "PasteFromGuest", menu_cb_paste },
    { "Fullscreen", menu_cb_fullscreen },
    { "ResizeTo", menu_cb_resize_to },
#ifdef USE_SMARTCARD
    { "InsertSmartcard", menu_cb_insert_smartcard },
    { "RemoveSmartcard", menu_cb_remove_smartcard },
#endif
#ifdef USE_USBREDIR
    { "SelectUsbDevices", menu_cb_select_usb_devices },
#endif
    { "MouseMode", menu_cb_mouse_mode },
    { "About", menu_cb_about },

    /* Toggle actions */
    { "grab-keyboard", NULL, NULL, "false", menu_cb_bool_prop },
    { "grab-mouse", NULL, NULL, "false", menu_cb_bool_prop },
    { "resize-guest", NULL, NULL, "false", menu_cb_bool_prop },
    { "scaling", NULL, NULL, "false", menu_cb_bool_prop },
    { "disable-inputs", NULL, NULL, "false", menu_cb_bool_prop },
    { "sync-modifiers", NULL, NULL, "false", menu_cb_bool_prop },
    { "auto-clipboard", NULL, NULL, "false", menu_cb_bool_prop },
    { "auto-usbredir", NULL, NULL, "false", menu_cb_bool_prop },
    { "Statusbar", NULL, NULL, "true", menu_cb_statusbar },
    { "Toolbar", NULL, NULL, "true", menu_cb_toolbar },

    /* Radio actions */
    { "preferred-compression", NULL, "s", "'auto-glz'", compression_cb },
    { "preferred-video-codec-type", NULL, "s", "'mjpeg'", video_codec_type_cb },
    { "recent-item", recent_item_activated_cb, "s", NULL, NULL }
};

static const char *spice_display_properties[] = {
    "grab-keyboard",
    "grab-mouse",
    "resize-guest",
    "scaling",
    "disable-inputs",
};

static const char *spice_gtk_session_properties[] = {
    "auto-clipboard",
    "auto-usbredir",
    "sync-modifiers",
};

typedef struct {
    const char *str;
    gint value;
} MenuMap;

static const MenuMap compression_entries[] = {
    { "auto-glz", SPICE_IMAGE_COMPRESSION_AUTO_GLZ },
    { "auto-lz",  SPICE_IMAGE_COMPRESSION_AUTO_LZ },
    { "quic",     SPICE_IMAGE_COMPRESSION_QUIC },
    { "glz",      SPICE_IMAGE_COMPRESSION_GLZ },
    { "lz",       SPICE_IMAGE_COMPRESSION_LZ },
#ifdef USE_LZ4
    { "lz4",      SPICE_IMAGE_COMPRESSION_LZ4 },
#endif
    { "off",      SPICE_IMAGE_COMPRESSION_OFF },
};

static const MenuMap video_codec_type_entries[] = {
    { "mjpeg", SPICE_VIDEO_CODEC_TYPE_MJPEG },
    { "vp8",   SPICE_VIDEO_CODEC_TYPE_VP8 },
    { "vp9",   SPICE_VIDEO_CODEC_TYPE_VP9 },
    { "h264",  SPICE_VIDEO_CODEC_TYPE_H264 },
};

static const char ui_xml[] =
"<interface>\n"
"  <menu id='main-menu'>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>File</attribute>\n"
"      <section>\n"
"        <item>\n"
"          <attribute name='label' translatable='yes'>Connect ...</attribute>\n"
"          <attribute name='action'>win.Connect</attribute>\n"
"        </item>\n"
"      </section>\n"
"      <section id='file-recent-section'/>\n"
"      <section>\n"
"        <item>\n"
"          <attribute name='label' translatable='yes'>Close</attribute>\n"
"          <attribute name='action'>win.Close</attribute>\n"
"        </item>\n"
"      </section>\n"
"    </submenu>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>Edit</attribute>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Copy to guest</attribute>\n"
"        <attribute name='action'>win.CopyToGuest</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F5</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Paste from guest</attribute>\n"
"        <attribute name='action'>win.PasteFromGuest</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F6</attribute>\n"
"      </item>\n"
"    </submenu>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>View</attribute>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Fullscreen</attribute>\n"
"        <attribute name='action'>win.Fullscreen</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F11</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Toolbar</attribute>\n"
"        <attribute name='action'>win.Toolbar</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Statusbar</attribute>\n"
"        <attribute name='action'>win.Statusbar</attribute>\n"
"      </item>\n"
"    </submenu>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>Input</attribute>\n"
#ifdef USE_SMARTCARD
"      <item>\n"
"        <attribute name='label' translatable='yes'>Insert Smartcard</attribute>\n"
"        <attribute name='action'>win.InsertSmartcard</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F8</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Remove Smartcard</attribute>\n"
"        <attribute name='action'>win.RemoveSmartcard</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F9</attribute>\n"
"      </item>\n"
#endif
#ifdef USE_USBREDIR
"      <item>\n"
"        <attribute name='label' translatable='yes'>Select USB Devices for redirection</attribute>\n"
"        <attribute name='action'>win.SelectUsbDevices</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F10</attribute>\n"
"      </item>\n"
#endif
"    </submenu>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>Options</attribute>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Grab keyboard when active and focused</attribute>\n"
"        <attribute name='action'>win.grab-keyboard</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Grab mouse in server mode (no tablet/vdagent)</attribute>\n"
"        <attribute name='action'>win.grab-mouse</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Toggle mouse mode</attribute>\n"
"        <attribute name='action'>win.MouseMode</attribute>\n"
"        <attribute name='accel'>&lt;Shift&gt;F7</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Resize guest to match window size</attribute>\n"
"        <attribute name='action'>win.resize-guest</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Scale display</attribute>\n"
"        <attribute name='action'>win.scaling</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Disable inputs</attribute>\n"
"        <attribute name='action'>win.disable-inputs</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Sync modifiers</attribute>\n"
"        <attribute name='action'>win.sync-modifiers</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Automatic clipboard sharing between host and guest</attribute>\n"
"        <attribute name='action'>win.auto-clipboard</attribute>\n"
"      </item>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>Auto redirect newly plugged in USB devices</attribute>\n"
"        <attribute name='action'>win.auto-usbredir</attribute>\n"
"      </item>\n"
"      <submenu id='compression-menu'>\n"
"        <attribute name='label' translatable='yes'>Preferred image compression</attribute>\n"
"        <item>\n"
"          <attribute name='label'>auto-glz</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>auto-glz</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>auto-lz</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>auto-lz</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>quic</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>quic</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>glz</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>glz</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>lz</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>lz</attribute>\n"
"        </item>\n"
#ifdef USE_LZ4
"        <item>\n"
"          <attribute name='label'>lz4</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>lz4</attribute>\n"
"        </item>\n"
#endif
"        <item>\n"
"          <attribute name='label'>off</attribute>\n"
"          <attribute name='action'>win.preferred-compression</attribute>\n"
"          <attribute name='target'>off</attribute>\n"
"        </item>\n"
"      </submenu>\n"
"      <submenu id='video-codec-type-menu'>\n"
"        <attribute name='label' translatable='yes'>Preferred video codec type</attribute>\n"
"        <item>\n"
"          <attribute name='label'>mjpeg</attribute>\n"
"          <attribute name='action'>win.preferred-video-codec-type</attribute>\n"
"          <attribute name='target'>mjpeg</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>vp8</attribute>\n"
"          <attribute name='action'>win.preferred-video-codec-type</attribute>\n"
"          <attribute name='target'>vp8</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>vp9</attribute>\n"
"          <attribute name='action'>win.preferred-video-codec-type</attribute>\n"
"          <attribute name='target'>vp9</attribute>\n"
"        </item>\n"
"        <item>\n"
"          <attribute name='label'>h264</attribute>\n"
"          <attribute name='action'>win.preferred-video-codec-type</attribute>\n"
"          <attribute name='target'>h264</attribute>\n"
"        </item>\n"
"      </submenu>\n"
"    </submenu>\n"
"    <submenu>\n"
"      <attribute name='label' translatable='yes'>Help</attribute>\n"
"      <item>\n"
"        <attribute name='label' translatable='yes'>About ...</attribute>\n"
"        <attribute name='action'>win.About</attribute>\n"
"      </item>\n"
"    </submenu>\n"
"  </menu>\n"
"</interface>\n";

static gboolean is_gtk_session_property(const gchar *property)
{
    int i;

    for (i = 0; i < G_N_ELEMENTS(spice_gtk_session_properties); i++) {
        if (g_strcmp0(spice_gtk_session_properties[i], property) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void recent_item_activated_cb(GSimpleAction *chooser,
                                     GVariant *variant,
 gpointer data)
{
    struct spice_connection *conn;
    const char *uri;
    
    uri = g_variant_get_string(variant, NULL);
    conn = connection_new();
    g_object_set(conn->session, "uri", uri, NULL);
    connection_connect(conn);
}

static void compression_cb(GSimpleAction *action,
                           GVariant *variant,
                           gpointer user_data)
{
    SpiceWindow *win = user_data;
    const char *comp_str = g_variant_get_string(variant, NULL);
    gint comp;
    guint i;

    for (i = 0; i < G_N_ELEMENTS(compression_entries); i++) {
        if (g_str_equal(comp_str, compression_entries[i].str)) {
            comp = compression_entries[i].value;
            break;
        }
    }
    g_simple_action_set_state(action, variant);
    g_key_file_set_string(keyfile, "general", "preferred-compression", comp_str);

    spice_display_channel_change_preferred_compression(SPICE_CHANNEL(win->display_channel),
                                                       comp);
}

static void video_codec_type_cb(GSimpleAction *action,
                                GVariant *variant,
                                gpointer user_data)
{
    SpiceWindow *win = user_data;
    static GArray *preferred_codecs = NULL;
    const char *codec_str = g_variant_get_string(variant, NULL);
    gint selected_codec;
    guint i;
    GError *err = NULL;

    for (i = 0; i < G_N_ELEMENTS(video_codec_type_entries); i++) {
        if (g_str_equal(codec_str, video_codec_type_entries[i].str)) {
            selected_codec = video_codec_type_entries[i].value;
            break;
        }
    }
    g_simple_action_set_state(action, variant);
    g_key_file_set_string(keyfile, "general", "preferred-video-codec-type", codec_str);

    if (!preferred_codecs) {
        preferred_codecs = g_array_sized_new(FALSE, FALSE,
                                             sizeof(gint),
                                             G_N_ELEMENTS(video_codec_type_entries));
        /* initialize with the menu ordering */
        for (i = 0; i < G_N_ELEMENTS(video_codec_type_entries); i++) {
            g_array_append_val(preferred_codecs, video_codec_type_entries[i].value);
        }
    }

    /* remove codec from array and insert at the beginning */
    for (i = 0; i < preferred_codecs->len &&
                g_array_index(preferred_codecs, gint, i) != selected_codec; i++);

    g_assert(i < preferred_codecs->len);
    g_array_remove_index(preferred_codecs, i);
    g_array_prepend_val(preferred_codecs, selected_codec);

    if (!spice_display_channel_change_preferred_video_codec_types(SPICE_CHANNEL(win->display_channel),
                                                                  (gint *) preferred_codecs->data,
                                                                  preferred_codecs->len, &err)) {
        g_warning("setting preferred video codecs failed: %s", err->message);
        g_error_free(err);
    }
}

static void
spice_window_class_init (SpiceWindowClass *klass)
{
}

static void
spice_window_init (SpiceWindow *self)
{
}

static SpiceWindow *create_spice_window(spice_connection *conn, SpiceChannel *channel, int id, gint monitor_id)
{
    char title[32];
    SpiceWindow *win;
    GAction *toggle;
    gboolean state;
    GtkWidget *vbox, *frame;
    GError *err = NULL;
    int i;
    SpiceGrabSequence *seq;

    win = g_object_new(SPICE_TYPE_WINDOW, NULL);
    win->id = id;
    win->monitor_id = monitor_id;
    win->conn = conn;
    win->display_channel = channel;

    /* toplevel */
    win->toplevel = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (spicy_title == NULL) {
        snprintf(title, sizeof(title), "spice display %d:%d", id, monitor_id);
    } else {
        snprintf(title, sizeof(title), "%s", spicy_title);
    }

    gtk_window_set_title(GTK_WINDOW(win->toplevel), title);
    g_signal_connect(G_OBJECT(win->toplevel), "window-state-event",
                     G_CALLBACK(window_state_cb), win);
    g_signal_connect(G_OBJECT(win->toplevel), "delete-event",
                     G_CALLBACK(delete_cb), win);

    /* menu */
    win->ag = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(win->ag), entries, G_N_ELEMENTS(entries), win);
    if (!spice_channel_test_capability(win->display_channel, SPICE_DISPLAY_CAP_PREF_COMPRESSION)) {
        GAction *compression_menu_action = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "CompressionMenu");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(compression_menu_action), FALSE);
    }
    if (!spice_channel_test_capability(win->display_channel,
                                       SPICE_DISPLAY_CAP_PREF_VIDEO_CODEC_TYPE)) {
        GAction *video_codec_type_menu_action =
            g_action_map_lookup_action(G_ACTION_MAP(win->ag), "VideoCodecTypeMenu");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(video_codec_type_menu_action), FALSE);
    }

    gtk_widget_insert_action_group(win->toplevel, "win", G_ACTION_GROUP(win->ag));
    win->ui = gtk_builder_new();

    if (!gtk_builder_add_from_string(win->ui, ui_xml, -1, &err)) {
        g_warning("building menus failed: %s", err->message);
        g_error_free(err);
        exit(1);
    }
    if (!hide_menu_bar) {
        GMenuModel *menu_model = G_MENU_MODEL(gtk_builder_get_object(win->ui, "main-menu"));
        win->menubar = gtk_menu_bar_new_from_model(menu_model);
    }

    /* recent menu */
    GMenu *recent_section = G_MENU(gtk_builder_get_object(win->ui, "file-recent-section"));
    if (recent_section) {
        GMenu *recent_menu = g_menu_new();
        GtkRecentManager *manager = gtk_recent_manager_get_default();
        GList *items = gtk_recent_manager_get_items(manager);
        GList *l;

        for (l = items; l != NULL; l = l->next) {
            GtkRecentInfo *info = l->data;
            const gchar *mime_type = gtk_recent_info_get_mime_type(info);

            if (mime_type && g_str_equal(mime_type, "application/x-spice")) {
                const gchar *uri = gtk_recent_info_get_uri(info);
                const gchar *name = gtk_recent_info_get_display_name(info);

                GMenuItem *item = g_menu_item_new(name, NULL);
                g_menu_item_set_action_and_target(item, "win.recent-item", "s", uri);
                g_menu_append_item(recent_menu, item);
                g_object_unref(item);
            }
        }

        g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);

        g_menu_append_submenu(recent_section, "Recent Files", G_MENU_MODEL(recent_menu));
        g_object_unref(recent_menu);
    }

    /* toolbar */
    GtkWidget *button;
    GtkStyleContext *context;

    win->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    context = gtk_widget_get_style_context(win->toolbar);
    gtk_style_context_add_class(context, "toolbar");

    button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_BUTTON);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.Close");
    gtk_container_add(GTK_CONTAINER(win->toolbar), button);

    gtk_container_add(GTK_CONTAINER(win->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    button = gtk_button_new_from_icon_name("edit-copy", GTK_ICON_SIZE_BUTTON);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.CopyToGuest");
    gtk_container_add(GTK_CONTAINER(win->toolbar), button);

    button = gtk_button_new_from_icon_name("edit-paste", GTK_ICON_SIZE_BUTTON);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.PasteFromGuest");
    gtk_container_add(GTK_CONTAINER(win->toolbar), button);

    gtk_container_add(GTK_CONTAINER(win->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    button = gtk_button_new_from_icon_name("view-fullscreen", GTK_ICON_SIZE_BUTTON);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.Fullscreen");
    gtk_container_add(GTK_CONTAINER(win->toolbar), button);

    gtk_container_add(GTK_CONTAINER(win->toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

    button = gtk_button_new_with_label("Resize to");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "win.ResizeTo");
    gtk_container_add(GTK_CONTAINER(win->toolbar), button);

/* spice display */
    win->spice = GTK_WIDGET(spice_display_new_with_monitor(conn->session, id, monitor_id));
    seq = spice_grab_sequence_new_from_string("Shift_L+F12");
    spice_display_set_grab_keys(SPICE_DISPLAY(win->spice), seq);
    spice_grab_sequence_free(seq);

    g_signal_connect(G_OBJECT(win->spice), "mouse-grab",
                     G_CALLBACK(mouse_grab_cb), win);
    g_signal_connect(G_OBJECT(win->spice), "keyboard-grab",
                     G_CALLBACK(keyboard_grab_cb), win);
    g_signal_connect(G_OBJECT(win->spice), "grab-keys-pressed",
                     G_CALLBACK(grab_keys_pressed_cb), win);

    /* status line */
    win->statusbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);

    win->status = gtk_label_new("status line");
    gtk_widget_set_halign(win->status, GTK_ALIGN_START);
    gtk_widget_set_valign(win->status, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(win->status, 3);
    gtk_widget_set_margin_end(win->status, 3);
    gtk_widget_set_margin_top(win->status, 1);
    gtk_widget_set_margin_bottom(win->status, 1);
    update_status_window(win);

    frame = gtk_frame_new(NULL);
    gtk_box_pack_start(GTK_BOX(win->statusbar), frame, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(frame), win->status);

    for (i = 0; i < STATE_MAX; i++) {
        win->st[i] = gtk_label_new("?");
        gtk_label_set_width_chars(GTK_LABEL(win->st[i]), 5);
        frame = gtk_frame_new(NULL);
        gtk_box_pack_end(GTK_BOX(win->statusbar), frame, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(frame), win->st[i]);
    }

    /* Make a vbox and put stuff in */
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 0);
    gtk_container_add(GTK_CONTAINER(win->toplevel), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), win->menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->toolbar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->spice, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(vbox), win->statusbar, FALSE, TRUE, 0);

    /* show window */
    if (fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(win->toplevel));

    gtk_widget_show_all(vbox);
    restore_configuration(win);

    /* init toggle actions */
    for (i = 0; i < G_N_ELEMENTS(spice_display_properties); i++) {
        toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag),
                                            spice_display_properties[i]);
        g_object_get(win->spice, spice_display_properties[i], &state, NULL);
        g_simple_action_set_state(G_SIMPLE_ACTION(toggle), g_variant_new_boolean(state));
    }

    for (i = 0; i < G_N_ELEMENTS(spice_gtk_session_properties); i++) {
        char notify[64];

        toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag),
                                            spice_gtk_session_properties[i]);
        g_object_get(win->conn->gtk_session, spice_gtk_session_properties[i],
                     &state, NULL);
        g_simple_action_set_state(G_SIMPLE_ACTION(toggle), g_variant_new_boolean(state));

        snprintf(notify, sizeof(notify), "notify::%s",
                 spice_gtk_session_properties[i]);
        spice_g_signal_connect_object(win->conn->gtk_session, notify,
                                      G_CALLBACK(menu_cb_conn_bool_prop_changed),
                                      win, 0);
    }

    update_edit_menu_window(win);

    toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "Toolbar");
    state = gtk_widget_get_visible(win->toolbar);
    g_simple_action_set_state(G_SIMPLE_ACTION(toggle), g_variant_new_boolean(state));

    toggle = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "Statusbar");
    state = gtk_widget_get_visible(win->statusbar);
    g_simple_action_set_state(G_SIMPLE_ACTION(toggle), g_variant_new_boolean(state));

#ifdef USE_SMARTCARD
    gboolean smartcard;

    enable_smartcard_actions(win, NULL, FALSE, FALSE);
    g_object_get(G_OBJECT(conn->session),
                 "enable-smartcard", &smartcard,
                 NULL);
    if (smartcard) {
        g_signal_connect(G_OBJECT(spice_smartcard_manager_get()), "reader-added",
                         (GCallback)reader_added_cb, win);
        g_signal_connect(G_OBJECT(spice_smartcard_manager_get()), "reader-removed",
                         (GCallback)reader_removed_cb, win);
        g_signal_connect(G_OBJECT(spice_smartcard_manager_get()), "card-inserted",
                         (GCallback)card_inserted_cb, win);
        g_signal_connect(G_OBJECT(spice_smartcard_manager_get()), "card-removed",
                         (GCallback)card_removed_cb, win);
    }
#endif

#ifndef USE_USBREDIR
    GAction *usbredir = g_action_map_lookup_action(G_ACTION_MAP(win->ag), "auto-usbredir");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(usbredir), FALSE);
#endif

    gtk_widget_grab_focus(win->spice);

    return win;
}

static void destroy_spice_window(SpiceWindow *win)
{
    if (win == NULL)
        return;

    SPICE_DEBUG("destroy window (#%d:%d)", win->id, win->monitor_id);
    g_object_unref(win->ag);
    g_object_unref(win->ui);
    gtk_widget_destroy(win->toplevel);
    g_object_unref(win);
}

/* ------------------------------------------------------------------ */

static void recent_add(SpiceSession *session)
{
    GtkRecentManager *recent;
    GtkRecentData meta = {
        .mime_type    = (char*)"application/x-spice",
        .app_name     = (char*)"spicy",
        .app_exec     = (char*)"spicy --uri=%u",
    };
    char *uri;

    g_object_get(session, "uri", &uri, NULL);
    SPICE_DEBUG("%s: %s", __FUNCTION__, uri);

    recent = gtk_recent_manager_get_default();
    if (g_str_has_prefix(uri, "spice://"))
        meta.display_name = uri + 8;
    else if (g_str_has_prefix(uri, "spice+unix://"))
        meta.display_name = uri + 13;
    else
        g_return_if_reached();

    if (!gtk_recent_manager_add_full(recent, uri, &meta))
        g_warning("Recent item couldn't be added successfully");

    g_free(uri);
}

static void main_channel_event(SpiceChannel *channel, SpiceChannelEvent event,
                               gpointer data)
{
    const GError *error = NULL;
    spice_connection *conn = data;
    char password[64];
    int rc;

    switch (event) {
    case SPICE_CHANNEL_OPENED:
        g_message("main channel: opened");
        recent_add(conn->session);
        break;
    case SPICE_CHANNEL_SWITCHING:
        g_message("main channel: switching host");
        break;
    case SPICE_CHANNEL_CLOSED:
        /* this event is only sent if the channel was succesfully opened before */
        g_message("main channel: closed");
        connection_disconnect(conn);
        break;
    case SPICE_CHANNEL_ERROR_IO:
        connection_disconnect(conn);
        break;
    case SPICE_CHANNEL_ERROR_TLS:
    case SPICE_CHANNEL_ERROR_LINK:
    case SPICE_CHANNEL_ERROR_CONNECT:
        error = spice_channel_get_error(channel);
        g_message("main channel: failed to connect");
        if (error) {
            g_message("channel error: %s", error->message);
        }

        if (spicy_connect_dialog(conn->session)) {
            connection_connect(conn);
        } else {
            connection_disconnect(conn);
        }
        break;
    case SPICE_CHANNEL_ERROR_AUTH:
        g_warning("main channel: auth failure (wrong password?)");
        memset(password, 0, sizeof(password));
        /* FIXME i18 */
        rc = ask_user(NULL, "Authentication",
                      "Please enter the spice server password",
                      password, sizeof(password), true);
        if (rc == 0) {
            g_object_set(conn->session, "password", password, NULL);
            connection_connect(conn);
        } else {
            connection_disconnect(conn);
        }
        break;
    default:
        /* TODO: more sophisticated error handling */
        g_warning("unknown main channel event: %u", event);
        /* connection_disconnect(conn); */
        break;
    }
}

static void main_mouse_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    gint mode;

    g_object_get(channel, "mouse-mode", &mode, NULL);
    switch (mode) {
    case SPICE_MOUSE_MODE_SERVER:
        conn->mouse_state = "server";
        break;
    case SPICE_MOUSE_MODE_CLIENT:
        conn->mouse_state = "client";
        break;
    default:
        conn->mouse_state = "?";
        break;
    }
    update_status(conn);
}

static void main_agent_update(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;

    g_object_get(channel, "agent-connected", &conn->agent_connected, NULL);
    conn->agent_state = conn->agent_connected ? "yes" : "no";
    update_status(conn);
    update_edit_menu(conn);
}

static void inputs_modifiers(SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int m, i;

    g_object_get(channel, "key-modifiers", &m, NULL);
    for (i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        if (conn->wins[i] == NULL)
            continue;

        gtk_label_set_text(GTK_LABEL(conn->wins[i]->st[STATE_SCROLL_LOCK]),
                           m & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK ? "SCROLL" : "");
        gtk_label_set_text(GTK_LABEL(conn->wins[i]->st[STATE_CAPS_LOCK]),
                           m & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK ? "CAPS" : "");
        gtk_label_set_text(GTK_LABEL(conn->wins[i]->st[STATE_NUM_LOCK]),
                           m & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK ? "NUM" : "");
    }
}

static void display_mark(SpiceChannel *channel, gint mark, SpiceWindow *win)
{
    g_return_if_fail(win != NULL);
    g_return_if_fail(win->toplevel != NULL);

    if (mark == TRUE) {
        gtk_widget_show(win->toplevel);
    } else {
        gtk_widget_hide(win->toplevel);
    }
}

static void update_auto_usbredir_sensitive(spice_connection *conn)
{
#ifdef USE_USBREDIR
    int i;
    GAction *ac;
    gboolean sensitive;

    sensitive = spice_session_has_channel_type(conn->session,
                                               SPICE_CHANNEL_USBREDIR);
    for (i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        if (conn->wins[i] == NULL)
            continue;
        ac = g_action_map_lookup_action(G_ACTION_MAP(conn->wins[i]->ag), "auto-usbredir");
        g_simple_action_set_enabled(G_SIMPLE_ACTION(ac), sensitive);
    }
#endif
}

static SpiceWindow* get_window(spice_connection *conn, int channel_id, int monitor_id)
{
    g_return_val_if_fail(channel_id < CHANNELID_MAX, NULL);
    g_return_val_if_fail(monitor_id < MONITORID_MAX, NULL);

    return conn->wins[channel_id * CHANNELID_MAX + monitor_id];
}

static void add_window(spice_connection *conn, SpiceWindow *win)
{
    g_return_if_fail(win != NULL);
    g_return_if_fail(win->id < CHANNELID_MAX);
    g_return_if_fail(win->monitor_id < MONITORID_MAX);
    g_return_if_fail(conn->wins[win->id * CHANNELID_MAX + win->monitor_id] == NULL);

    SPICE_DEBUG("add display monitor %d:%d", win->id, win->monitor_id);
    conn->wins[win->id * CHANNELID_MAX + win->monitor_id] = win;
}

static void del_window(spice_connection *conn, SpiceWindow *win)
{
    if (win == NULL)
        return;

    g_return_if_fail(win->id < CHANNELID_MAX);
    g_return_if_fail(win->monitor_id < MONITORID_MAX);

    g_debug("del display monitor %d:%d", win->id, win->monitor_id);
    conn->wins[win->id * CHANNELID_MAX + win->monitor_id] = NULL;
    if (win->id > 0)
        spice_main_channel_update_display_enabled(conn->main, win->id, FALSE, TRUE);
    else
        spice_main_channel_update_display_enabled(conn->main, win->monitor_id, FALSE, TRUE);
    spice_main_channel_send_monitor_config(conn->main);

    destroy_spice_window(win);
}

static void display_monitors(SpiceChannel *display, GParamSpec *pspec,
                             spice_connection *conn)
{
    GArray *monitors = NULL;
    int id;
    guint i;

    g_object_get(display,
                 "channel-id", &id,
                 "monitors", &monitors,
                 NULL);
    g_return_if_fail(monitors != NULL);

    for (i = 0; i < monitors->len; i++) {
        SpiceWindow *w;

        if (!get_window(conn, id, i)) {
            w = create_spice_window(conn, display, id, i);
            add_window(conn, w);
            spice_g_signal_connect_object(display, "display-mark",
                                          G_CALLBACK(display_mark), w, 0);
            gtk_widget_show(w->toplevel);
            update_auto_usbredir_sensitive(conn);
        }
    }

    for (; i < MONITORID_MAX; i++)
        del_window(conn, get_window(conn, id, i));

    g_clear_pointer(&monitors, g_array_unref);
}

static void port_write_cb(GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
    SpicePortChannel *port = SPICE_PORT_CHANNEL(source_object);
    GError *error = NULL;

    spice_port_channel_write_finish(port, res, &error);
    if (error != NULL)
        g_warning("%s", error->message);
    g_clear_error(&error);
}

static void port_flushed_cb(GObject *source_object,
                            GAsyncResult *res,
                            gpointer user_data)
{
    SpiceChannel *channel = SPICE_CHANNEL(source_object);
    GError *error = NULL;

    spice_channel_flush_finish(channel, res, &error);
    if (error != NULL)
        g_warning("%s", error->message);
    g_clear_error(&error);

    spice_channel_disconnect(channel, SPICE_CHANNEL_CLOSED);
}

static gboolean input_cb(GIOChannel *gin, GIOCondition condition, gpointer data)
{
    char buf[4096];
    gsize bytes_read;
    GIOStatus status;

    if (!(condition & G_IO_IN))
        return FALSE;

    status = g_io_channel_read_chars(gin, buf, sizeof(buf), &bytes_read, NULL);
    if (status != G_IO_STATUS_NORMAL)
        return FALSE;

    if (stdin_port != NULL)
        spice_port_channel_write_async(stdin_port, buf, bytes_read, NULL, port_write_cb, NULL);

    return TRUE;
}

static void watch_stdin(void);

static void port_opened(SpiceChannel *channel, GParamSpec *pspec,
                        spice_connection *conn)
{
    SpicePortChannel *port = SPICE_PORT_CHANNEL(channel);
    gchar *name = NULL;
    gboolean opened = FALSE;

    g_object_get(channel,
                 "port-name", &name,
                 "port-opened", &opened,
                 NULL);

    g_printerr("port %p %s: %s\n", channel, name, opened ? "opened" : "closed");

    if (opened) {
        /* only send a break event and disconnect */
        if (g_strcmp0(name, "org.spice.spicy.break") == 0) {
            spice_port_channel_event(port, SPICE_PORT_EVENT_BREAK);
            spice_channel_flush_async(channel, NULL, port_flushed_cb, conn);
        }

        /* handle the first spicy port and connect it to stdin/out */
        if (g_strcmp0(name, "org.spice.spicy") == 0 && stdin_port == NULL) {
            watch_stdin();
            stdin_port = port;
        }
    } else {
        if (port == stdin_port)
            stdin_port = NULL;
    }

    g_free(name);
}

static void port_data(SpicePortChannel *port,
                      gpointer data, int size, spice_connection *conn)
{
    int r;

    if (port != stdin_port)
        return;

    r = write(fileno(stdout), data, size);
    if (r != size) {
        g_warning("port write failed result %d/%d errno %d", r, size, errno);
    }
}

typedef struct {
    GtkWidget *vbox;
    GtkWidget *hbox;
    GtkWidget *progress;
    GtkWidget *label;
    GtkWidget *cancel;
} TransferTaskWidgets;

static void transfer_update_progress(GObject *object,
                                     GParamSpec *pspec,
                                     gpointer user_data)
{
    spice_connection *conn = user_data;
    TransferTaskWidgets *widgets = g_hash_table_lookup(conn->transfers, object);
    g_return_if_fail(widgets);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(widgets->progress),
                                  spice_file_transfer_task_get_progress(SPICE_FILE_TRANSFER_TASK(object)));
}

static void transfer_task_finished(SpiceFileTransferTask *task, GError *error, spice_connection *conn)
{
    if (error)
        g_warning("%s", error->message);
    g_hash_table_remove(conn->transfers, task);
    if (!g_hash_table_size(conn->transfers))
        gtk_widget_hide(conn->transfer_dialog);
}

static gboolean dialog_delete_cb(GtkWidget *widget,
                                 GdkEvent *event G_GNUC_UNUSED,
                                 gpointer user_data G_GNUC_UNUSED)
{
    gtk_dialog_response(GTK_DIALOG(widget), GTK_RESPONSE_CANCEL);
    return TRUE;
}

static void dialog_response_cb(GtkDialog *dialog,
                               gint response_id,
                               gpointer user_data)
{
    spice_connection *conn = user_data;
    g_print("Reponse: %i\n", response_id);

    if (response_id == GTK_RESPONSE_CANCEL) {
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init(&iter, conn->transfers);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            SpiceFileTransferTask *task = key;
            spice_file_transfer_task_cancel(task);
        }
    }
}

static void
task_cancel_cb(GtkButton *button,
               gpointer user_data)
{
    SpiceFileTransferTask *task = SPICE_FILE_TRANSFER_TASK(user_data);
    spice_file_transfer_task_cancel(task);
}

static TransferTaskWidgets *
transfer_task_widgets_new(SpiceFileTransferTask *task)
{
    char *filename;
    TransferTaskWidgets *widgets = g_new0(TransferTaskWidgets, 1);

    widgets->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    widgets->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    widgets->cancel = gtk_button_new_with_label("Cancel");

    widgets->progress = gtk_progress_bar_new();
    filename = spice_file_transfer_task_get_filename(task);
    widgets->label = gtk_label_new(filename);
    g_free(filename);

    gtk_widget_set_halign(widgets->label, GTK_ALIGN_START);
    gtk_widget_set_valign(widgets->label, GTK_ALIGN_BASELINE);
    gtk_widget_set_valign(widgets->progress, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(widgets->progress, TRUE);
    gtk_widget_set_valign(widgets->cancel, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(widgets->progress, FALSE);

    gtk_box_pack_start(GTK_BOX(widgets->hbox), widgets->progress,
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(widgets->hbox), widgets->cancel,
                       FALSE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(widgets->vbox), widgets->label,
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(widgets->vbox), widgets->hbox,
                       TRUE, TRUE, 0);

    g_signal_connect(widgets->cancel, "clicked",
                     G_CALLBACK(task_cancel_cb), task);

    gtk_widget_show_all(widgets->vbox);

    return widgets;
}

static void
transfer_task_widgets_free(TransferTaskWidgets *widgets)
{
    /* child widgets will be destroyed automatically */
    gtk_widget_destroy(widgets->vbox);
    g_free(widgets);
}

static void spice_connection_add_task(spice_connection *conn, SpiceFileTransferTask *task)
{
    TransferTaskWidgets *widgets;
    GtkWidget *content = NULL;

    g_signal_connect(task, "notify::progress",
                     G_CALLBACK(transfer_update_progress), conn);
    g_signal_connect(task, "finished",
                     G_CALLBACK(transfer_task_finished), conn);
    if (!conn->transfer_dialog) {
        conn->transfer_dialog = gtk_dialog_new_with_buttons("File Transfers",
                                                            GTK_WINDOW(conn->wins[0]->toplevel), 0,
                                                            "Cancel", GTK_RESPONSE_CANCEL, NULL);
        gtk_dialog_set_default_response(GTK_DIALOG(conn->transfer_dialog),
                                        GTK_RESPONSE_CANCEL);
        gtk_window_set_resizable(GTK_WINDOW(conn->transfer_dialog), FALSE);
        g_signal_connect(conn->transfer_dialog, "response",
                         G_CALLBACK(dialog_response_cb), conn);
        g_signal_connect(conn->transfer_dialog, "delete-event",
                         G_CALLBACK(dialog_delete_cb), conn);
    }
    gtk_widget_show(conn->transfer_dialog);
    content = gtk_dialog_get_content_area(GTK_DIALOG(conn->transfer_dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    widgets = transfer_task_widgets_new(task);
    g_hash_table_insert(conn->transfers, g_object_ref(task), widgets);
    gtk_box_pack_start(GTK_BOX(content),
                       widgets->vbox, TRUE, TRUE, 6);
}

static void new_file_transfer(SpiceMainChannel *main, SpiceFileTransferTask *task, gpointer user_data)
{
    spice_connection *conn = user_data;
    g_debug("new file transfer task");
    spice_connection_add_task(conn, task);
}

static void channel_new(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int id;

    g_object_get(channel, "channel-id", &id, NULL);
    SPICE_DEBUG("new channel (#%d)", id);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        SPICE_DEBUG("new main channel");
        conn->main = SPICE_MAIN_CHANNEL(channel);
        g_signal_connect(channel, "channel-event",
                         G_CALLBACK(main_channel_event), conn);
        g_signal_connect(channel, "main-mouse-update",
                         G_CALLBACK(main_mouse_update), conn);
        g_signal_connect(channel, "main-agent-update",
                         G_CALLBACK(main_agent_update), conn);
        g_signal_connect(channel, "new-file-transfer",
                         G_CALLBACK(new_file_transfer), conn);
        main_mouse_update(channel, conn);
        main_agent_update(channel, conn);
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        if (id >= SPICE_N_ELEMENTS(conn->wins))
            return;
        if (conn->wins[id] != NULL)
            return;
        SPICE_DEBUG("new display channel (#%d)", id);
        g_signal_connect(channel, "notify::monitors",
                         G_CALLBACK(display_monitors), conn);
        spice_channel_connect(channel);
    }

    if (SPICE_IS_INPUTS_CHANNEL(channel)) {
        SPICE_DEBUG("new inputs channel");
        g_signal_connect(channel, "inputs-modifiers",
                         G_CALLBACK(inputs_modifiers), conn);
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        SPICE_DEBUG("new audio channel");
        conn->audio = spice_audio_get(s, NULL);
    }

    if (SPICE_IS_USBREDIR_CHANNEL(channel)) {
        update_auto_usbredir_sensitive(conn);
    }

    if (SPICE_IS_PORT_CHANNEL(channel)) {
        g_signal_connect(channel, "notify::port-opened",
                         G_CALLBACK(port_opened), conn);
        g_signal_connect(channel, "port-data",
                         G_CALLBACK(port_data), conn);
        spice_channel_connect(channel);
    }
}

static void channel_destroy(SpiceSession *s, SpiceChannel *channel, gpointer data)
{
    spice_connection *conn = data;
    int id;

    g_object_get(channel, "channel-id", &id, NULL);
    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        SPICE_DEBUG("zap main channel");
        conn->main = NULL;
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        if (id >= SPICE_N_ELEMENTS(conn->wins))
            return;
        SPICE_DEBUG("zap display channel (#%d)", id);
        /* FIXME destroy widget only */
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        SPICE_DEBUG("zap audio channel");
    }

    if (SPICE_IS_USBREDIR_CHANNEL(channel)) {
        update_auto_usbredir_sensitive(conn);
    }

    if (SPICE_IS_PORT_CHANNEL(channel)) {
        if (SPICE_PORT_CHANNEL(channel) == stdin_port)
            stdin_port = NULL;
    }
}

static void migration_state(GObject *session,
                            GParamSpec *pspec, gpointer data)
{
    SpiceSessionMigration mig;

    g_object_get(session, "migration-state", &mig, NULL);
    if (mig == SPICE_SESSION_MIGRATION_SWITCHING)
        g_message("migrating session");
}

static spice_connection *connection_new(void)
{
    spice_connection *conn;
    SpiceUsbDeviceManager *manager;

    conn = g_new0(spice_connection, 1);
    conn->session = spice_session_new();
    conn->gtk_session = spice_gtk_session_get(conn->session);
    g_signal_connect(conn->session, "channel-new",
                     G_CALLBACK(channel_new), conn);
    g_signal_connect(conn->session, "channel-destroy",
                     G_CALLBACK(channel_destroy), conn);
    g_signal_connect(conn->session, "notify::migration-state",
                     G_CALLBACK(migration_state), conn);
    g_signal_connect(conn->session, "disconnected",
                     G_CALLBACK(connection_destroy), conn);

    manager = spice_usb_device_manager_get(conn->session, NULL);
    if (manager) {
        g_signal_connect(manager, "auto-connect-failed",
                         G_CALLBACK(usb_connect_failed), NULL);
        g_signal_connect(manager, "device-error",
                         G_CALLBACK(usb_connect_failed), NULL);
    }

    conn->transfers = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            g_object_unref,
                                            (GDestroyNotify)transfer_task_widgets_free);
    connections++;
    SPICE_DEBUG("%s (%d)", __FUNCTION__, connections);
    return conn;
}

static void connection_connect(spice_connection *conn)
{
    conn->disconnecting = false;
    spice_session_connect(conn->session);
}

static void connection_disconnect(spice_connection *conn)
{
    if (conn->disconnecting)
        return;
    conn->disconnecting = true;
    spice_session_disconnect(conn->session);
}

static void connection_destroy(SpiceSession *session,
                               spice_connection *conn)
{
    for (int i = 0; i < SPICE_N_ELEMENTS(conn->wins); i++) {
        destroy_spice_window(conn->wins[i]);
    }
    g_object_unref(conn->session);
    g_hash_table_unref(conn->transfers);
    g_free(conn);

    connections--;
    SPICE_DEBUG("%s (%d)", __FUNCTION__, connections);
    if (connections > 0) {
        return;
    }

    g_main_loop_quit(mainloop);
}

/* ------------------------------------------------------------------ */

static GOptionEntry cmd_entries[] = {
    {
        .long_name        = "full-screen",
        .short_name       = 'f',
        .arg              = G_OPTION_ARG_NONE,
        .arg_data         = &fullscreen,
        .description      = "Open in full screen mode",
    },{
        .long_name        = "hide-menu-bar",
        .arg              = G_OPTION_ARG_NONE,
        .arg_data         = &hide_menu_bar,
        .description      = "Hide menu bar",
    },{
        .long_name        = "version",
        .arg              = G_OPTION_ARG_NONE,
        .arg_data         = &version,
        .description      = "Display version and quit",
    },{
        .long_name        = "title",
        .arg              = G_OPTION_ARG_STRING,
        .arg_data         = &spicy_title,
        .description      = "Set the window title",
        .arg_description  = "<title>",
    },{
        /* end of list */
    }
};

static void usb_connect_failed(GObject               *object,
                               SpiceUsbDevice        *device,
                               GError                *error,
                               gpointer               data)
{
    GtkWidget *dialog;

    if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_CANCELLED)
        return;

    dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                                    GTK_BUTTONS_CLOSE,
                                    "USB redirection error");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
                                             "%s", error->message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void setup_terminal(gboolean reset)
{
    int stdinfd = fileno(stdin);

    if (!isatty(stdinfd))
        return;

#ifdef HAVE_TERMIOS_H
    struct termios tios;
    static struct termios saved_tios;
    static bool saved = false;

    if (reset) {
        if (!saved)
            return;
        tios = saved_tios;
    } else {
        tcgetattr(stdinfd, &tios);
        saved_tios = tios;
        saved = true;
        tios.c_lflag &= ~(ICANON | ECHO);
    }

    tcsetattr(stdinfd, TCSANOW, &tios);
#endif
}

static void watch_stdin(void)
{
    int stdinfd = fileno(stdin);
    GIOChannel *gin;

    setup_terminal(false);
    gin = g_io_channel_unix_new(stdinfd);
    g_io_channel_set_flags(gin, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(gin, G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL, input_cb, NULL);
}

int main(int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    spice_connection *conn;
    gchar *conf_file, *conf;
    char *host = NULL, *port = NULL, *tls_port = NULL, *unix_path = NULL;

    keyfile = g_key_file_new();

    int mode = S_IRWXU;
    conf_file = g_build_filename(g_get_user_config_dir(), "spicy", NULL);
    if (g_mkdir_with_parents(conf_file, mode) == -1)
        SPICE_DEBUG("failed to create config directory");
    g_free(conf_file);

    conf_file = g_build_filename(g_get_user_config_dir(), "spicy", "settings", NULL);
    if (!g_key_file_load_from_file(keyfile, conf_file,
                                   G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, &error)) {
        SPICE_DEBUG("Couldn't load configuration: %s", error->message);
        g_clear_error(&error);
    }

    /* parse opts */
    gtk_init(&argc, &argv);
    context = g_option_context_new("- spice client test application");
    g_option_context_set_summary(context, "Gtk+ test client to connect to Spice servers.");
    g_option_context_set_description(context, "Report bugs to " PACKAGE_BUGREPORT ".");
    g_option_context_add_group(context, spice_get_option_group());
    g_option_context_set_main_group(context, spice_cmdline_get_option_group());
    g_option_context_add_main_entries(context, cmd_entries, NULL);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse (context, &argc, &argv, &error)) {
        g_print("option parsing failed: %s\n", error->message);
        exit(1);
    }
    g_option_context_free(context);

    if (version) {
        g_print("spicy " PACKAGE_VERSION "\n");
        exit(0);
    }

    mainloop = g_main_loop_new(NULL, false);

    conn = connection_new();
    spice_set_session_option(conn->session);
    spice_cmdline_session_setup(conn->session);

    g_object_get(conn->session,
                 "unix-path", &unix_path,
                 "host", &host,
                 "port", &port,
                 "tls-port", &tls_port,
                 NULL);
    /* If user doesn't provide hostname and port, show the dialog window
       instead of connecting to server automatically */
    if ((host == NULL || (port == NULL && tls_port == NULL)) && unix_path == NULL) {
        if (!spicy_connect_dialog(conn->session)) {
            exit(0);
        }
    }
    g_free(host);
    g_free(port);
    g_free(tls_port);
    g_free(unix_path);

    connection_connect(conn);
    if (connections > 0)
        g_main_loop_run(mainloop);
    g_main_loop_unref(mainloop);

    if ((conf = g_key_file_to_data(keyfile, NULL, &error)) == NULL ||
        !g_file_set_contents(conf_file, conf, -1, &error)) {
        SPICE_DEBUG("Couldn't save configuration: %s", error->message);
        g_error_free(error);
        error = NULL;
    }

    g_free(conf_file);
    g_free(conf);
    g_key_file_free(keyfile);

    g_free(spicy_title);

    setup_terminal(true);
    gst_deinit();
    return 0;
}
