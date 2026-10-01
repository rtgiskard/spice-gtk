/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Minimal GTK4 test client for libspice-client-gtk-4.0

   This is a stripped-down SPICE client for verifying the GTK4 port.
   It connects to a SPICE server, displays the remote desktop in a
   window, and supports keyboard/mouse input, clipboard sharing,
   audio, and basic USB redirection.

   Usage:
     spicy-test-gtk4 --uri spice://host?port=5900
     spicy-test-gtk4 -h host -p port [-w password]
     spicy-test-gtk4 --uri spice://host?port=5900 --spice-debug

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "config.h"
#include <glib/gi18n-lib.h>
#include <gtk/gtk.h>

#include "spice-client.h"
#include "spice-widget.h"
#include "spice-gtk-session.h"
#include "spice-audio.h"
#include "spice-option.h"
#include "spice-cmdline.h"

/* Application state */
typedef struct {
    GtkApplication *app;
    SpiceSession   *session;
    SpiceAudio     *audio;

    GtkWidget      *window;
    GtkWidget      *display;

    gboolean        connected;
    int             channel_id;
} TestClient;

static TestClient client = { 0 };

/* ------------------------------------------------------------------ */
/* Channel event handler — reports connection status                   */
/* ------------------------------------------------------------------ */
static void
main_channel_event_cb(SpiceChannel *channel, SpiceChannelEvent event,
                      gpointer data G_GNUC_UNUSED)
{
    const char *msg = NULL;

    switch (event) {
    case SPICE_CHANNEL_OPENED:
        g_message("Main channel opened — connected to SPICE server");
        client.connected = TRUE;
        return;
    case SPICE_CHANNEL_SWITCHING:
        g_message("Switching host (migration)...");
        return;
    case SPICE_CHANNEL_CLOSED:
        msg = "Connection closed";
        break;
    case SPICE_CHANNEL_ERROR_CONNECT:
        msg = "Connection error — could not reach server";
        break;
    case SPICE_CHANNEL_ERROR_TLS:
        msg = "TLS error";
        break;
    case SPICE_CHANNEL_ERROR_LINK:
        msg = "Link error";
        break;
    case SPICE_CHANNEL_ERROR_AUTH:
        msg = "Authentication failed (wrong password?)";
        break;
    case SPICE_CHANNEL_ERROR_IO:
        msg = "I/O error";
        break;
    default:
        msg = "Unknown channel event";
        break;
    }

    g_warning("%s (event %d)", msg, event);
}

/* ------------------------------------------------------------------ */
/* Display mark — show window once guest has painted something        */
/* ------------------------------------------------------------------ */
static void
display_mark_cb(SpiceChannel *channel, gint mark, gpointer data G_GNUC_UNUSED)
{
    if (client.window) {
        gtk_widget_set_visible(client.window, mark != 0);
    }
}

/* ------------------------------------------------------------------ */
/* Grab notification callbacks                                        */
/* ------------------------------------------------------------------ */
static void
mouse_grab_cb(SpiceDisplay *display G_GNUC_UNUSED, gint grabbed,
              gpointer data G_GNUC_UNUSED)
{
    if (grabbed)
        g_message("Mouse grabbed — press Ctrl+Alt to release");
    else
        g_message("Mouse released");
}

static void
keyboard_grab_cb(SpiceDisplay *display G_GNUC_UNUSED, gint grabbed,
                 gpointer data G_GNUC_UNUSED)
{
    if (grabbed)
        g_message("Keyboard grabbed");
    else
        g_message("Keyboard released");
}

/* ------------------------------------------------------------------ */
/* Monitor configuration — create the display widget and window       */
/* ------------------------------------------------------------------ */
static void
create_display_window(int channel_id)
{
    if (client.display != NULL)
        return; /* already created */

    g_message("Creating display for channel %d", channel_id);

    /* Create the SpiceDisplay widget */
    client.display = GTK_WIDGET(spice_display_new(client.session, channel_id));
    g_object_set(client.display,
                 "scaling", TRUE,
                 "resize-guest", TRUE,
                 "grab-keyboard", TRUE,
                 "grab-mouse", TRUE,
                 NULL);

    g_signal_connect(client.display, "mouse-grab",
                     G_CALLBACK(mouse_grab_cb), NULL);
    g_signal_connect(client.display, "keyboard-grab",
                     G_CALLBACK(keyboard_grab_cb), NULL);

    /* Create the top-level window */
    client.window = gtk_window_new();
    gtk_window_set_application(GTK_WINDOW(client.window), client.app);
    gtk_window_set_title(GTK_WINDOW(client.window), "SPICE GTK4 Test");
    gtk_window_set_default_size(GTK_WINDOW(client.window), 1024, 768);

    /* Pack display widget into window */
    gtk_window_set_child(GTK_WINDOW(client.window), client.display);

    /* Present the window and give focus to the display */
    gtk_window_present(GTK_WINDOW(client.window));
    gtk_widget_grab_focus(client.display);

    client.channel_id = channel_id;
}

static void
display_monitors_cb(SpiceChannel *channel, GParamSpec *pspec G_GNUC_UNUSED,
                    gpointer data G_GNUC_UNUSED)
{
    int id;
    g_object_get(channel, "channel-id", &id, NULL);

    GArray *monitors = NULL;
    g_object_get(channel, "monitors", &monitors, NULL);

    if (monitors == NULL || monitors->len == 0) {
        g_message("Display channel %d: no monitors", id);
        if (monitors)
            g_array_unref(monitors);
        return;
    }

    g_message("Display channel %d: %d monitor(s)", id, monitors->len);
    g_array_unref(monitors);

    /* Only handle first display channel */
    create_display_window(id);
}

/* ------------------------------------------------------------------ */
/* Channel lifecycle                                                  */
/* ------------------------------------------------------------------ */
static void
channel_new_cb(SpiceSession *session, SpiceChannel *channel,
               gpointer data G_GNUC_UNUSED)
{
    int id;
    g_object_get(channel, "channel-id", &id, NULL);
    g_message("New channel: %s (id=%d)", G_OBJECT_TYPE_NAME(channel), id);

    if (SPICE_IS_MAIN_CHANNEL(channel)) {
        g_signal_connect(channel, "channel-event",
                         G_CALLBACK(main_channel_event_cb), NULL);
    }

    if (SPICE_IS_DISPLAY_CHANNEL(channel)) {
        g_signal_connect(channel, "notify::monitors",
                         G_CALLBACK(display_monitors_cb), NULL);
        g_signal_connect(channel, "display-mark",
                         G_CALLBACK(display_mark_cb), NULL);
        spice_channel_connect(channel);
    }

    if (SPICE_IS_PLAYBACK_CHANNEL(channel)) {
        if (client.audio == NULL)
            client.audio = spice_audio_get(session, NULL);
    }
}

static void
channel_destroy_cb(SpiceSession *session G_GNUC_UNUSED,
                   SpiceChannel *channel,
                   gpointer data G_GNUC_UNUSED)
{
    int id;
    g_object_get(channel, "channel-id", &id, NULL);
    g_message("Channel destroyed: %s (id=%d)", G_OBJECT_TYPE_NAME(channel), id);

    if (SPICE_IS_DISPLAY_CHANNEL(channel) && client.display) {
        int ch_id;
        g_object_get(channel, "channel-id", &ch_id, NULL);
        if (ch_id == client.channel_id) {
            client.display = NULL;
            if (client.window) {
                gtk_window_destroy(GTK_WINDOW(client.window));
                client.window = NULL;
            }
        }
    }
}

static void
session_disconnected_cb(SpiceSession *session G_GNUC_UNUSED,
                        gpointer data G_GNUC_UNUSED)
{
    g_message("Disconnected from SPICE server");
    client.connected = FALSE;
    client.display = NULL;
    client.window = NULL;
    client.audio = NULL;

    /* Quit the application */
    if (client.app)
        g_application_quit(G_APPLICATION(client.app));
}

/* ------------------------------------------------------------------ */
/* GtkApplication activate — set up session and connect               */
/* ------------------------------------------------------------------ */
static void
app_activate_cb(GtkApplication *app, gpointer data G_GNUC_UNUSED)
{
    client.app = app;

    /* Keep the application alive until we explicitly quit —
       without this, GtkApplication exits when no windows are open */
    g_application_hold(G_APPLICATION(app));

    /* Create session */
    client.session = spice_session_new();
    spice_set_session_option(client.session);
    spice_cmdline_session_setup(client.session);

    /* Verify we have connection parameters */
    gchar *uri = NULL, *host = NULL;
    g_object_get(client.session, "uri", &uri, "host", &host, NULL);
    if (!uri && !host) {
        g_printerr("Error: no server specified. Use --uri or -h/-p options.\n"
                    "Try --help for usage information.\n");
        g_application_quit(G_APPLICATION(app));
        g_free(uri);
        g_free(host);
        return;
    }
    g_free(uri);
    g_free(host);

    /* Set up GTK session for clipboard and modifier sync */
    SpiceGtkSession *gtk_session = spice_gtk_session_get(client.session);
    g_object_set(gtk_session,
                 "auto-clipboard", TRUE,
                 "sync-modifiers", TRUE,
                 NULL);

    /* Connect session signals */
    g_signal_connect(client.session, "channel-new",
                     G_CALLBACK(channel_new_cb), NULL);
    g_signal_connect(client.session, "channel-destroy",
                     G_CALLBACK(channel_destroy_cb), NULL);
    g_signal_connect(client.session, "disconnected",
                     G_CALLBACK(session_disconnected_cb), NULL);

    g_message("Connecting to SPICE server...");
    if (!spice_session_connect(client.session)) {
        g_printerr("Error: spice_session_connect() failed.\n");
        g_application_quit(G_APPLICATION(app));
        return;
    }
}

static void
app_shutdown_cb(GtkApplication *app G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
    if (client.session) {
        spice_session_disconnect(client.session);
        g_clear_object(&client.session);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
int
main(int argc, char *argv[])
{
    GError *error = NULL;

    /* Parse command-line options before GtkApplication takes over */
    GOptionContext *context = g_option_context_new("- SPICE GTK4 test client");
    g_option_context_add_group(context, spice_get_option_group());
    g_option_context_set_main_group(context, spice_cmdline_get_option_group());

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        g_error_free(error);
        return 1;
    }
    g_option_context_free(context);

    GtkApplication *app = gtk_application_new("org.spice.gtk4test",
                                              G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(app_activate_cb), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(app_shutdown_cb), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}
