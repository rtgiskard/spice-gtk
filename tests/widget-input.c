#include "spice-widget-priv.h"
#include <gdk/x11/gdkx.h>
#include <X11/extensions/XTest.h>

typedef struct {
    SpiceSession *session;
    SpiceSession *channel_session;
    SpiceDisplay *display;
    GtkWindow *window;
    Display *xdisplay;
    guint button_events;
} InputFixture;

static void iterate_display(void)
{
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_usleep(1000);
}

static gboolean observe_buttons(GtkEventControllerLegacy *controller G_GNUC_UNUSED,
                                 GdkEvent *event, gpointer user_data)
{
    InputFixture *fixture = user_data;
    GdkEventType type = gdk_event_get_event_type(event);

    if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE)
        fixture->button_events++;
    return GDK_EVENT_PROPAGATE;
}

static void input_setup(InputFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *above = gtk_drawing_area_new();
    GtkWidget *beside = gtk_drawing_area_new();
    GtkEventController *observer = gtk_event_controller_legacy_new();
    gint64 deadline;

    fixture->session = spice_session_new();
    fixture->channel_session = spice_session_new();
    g_object_set(spice_gtk_session_get(fixture->session), "auto-clipboard", FALSE, NULL);
    fixture->display = spice_display_new(fixture->session, 0);
    g_object_ref_sink(fixture->display);
    g_object_set(fixture->display, "grab-keyboard", FALSE, "grab-mouse", FALSE,
                 "scaling", FALSE, "resize-guest", FALSE, NULL);
    /* These real channels stay disconnected; no VM or socket is involved. */
    fixture->display->priv->inputs = SPICE_INPUTS_CHANNEL(
        spice_channel_new(fixture->channel_session, SPICE_CHANNEL_INPUTS, 0));
    fixture->display->priv->mouse_mode = SPICE_MOUSE_MODE_CLIENT;
    fixture->window = GTK_WINDOW(gtk_window_new());
    gtk_widget_set_size_request(above, 1, 96);
    gtk_widget_set_size_request(beside, 128, 1);
    gtk_widget_set_size_request(GTK_WIDGET(fixture->display), 320, 240);
    gtk_box_append(GTK_BOX(column), above);
    gtk_box_append(GTK_BOX(column), row);
    gtk_box_append(GTK_BOX(row), beside);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(fixture->display));
    gtk_window_set_child(fixture->window, column);
    /* Observe real events after the widget, also checking that its controller
     * does not consume buttons that an embedding parent needs to receive. */
    gtk_event_controller_set_propagation_phase(observer, GTK_PHASE_BUBBLE);
    g_signal_connect(observer, "event", G_CALLBACK(observe_buttons), fixture);
    gtk_widget_add_controller(GTK_WIDGET(fixture->window), observer);
    gtk_window_present(fixture->window);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!gtk_widget_get_mapped(GTK_WIDGET(fixture->display)) ||
           gdk_surface_get_width(gtk_native_get_surface(GTK_NATIVE(fixture->window))) < 448 ||
           gdk_surface_get_height(gtk_native_get_surface(GTK_NATIVE(fixture->window))) < 336) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
    fixture->display->priv->area = (GdkRectangle) { 0, 0, 100, 80 };
    fixture->xdisplay = gdk_x11_display_get_xdisplay(
        gtk_widget_get_display(GTK_WIDGET(fixture->display)));
}

static void input_teardown(InputFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    gtk_window_destroy(fixture->window);
    g_clear_object(&fixture->display);
    g_clear_object(&fixture->channel_session);
    g_clear_object(&fixture->session);
}

static void move_pointer(InputFixture *fixture, double x, double y)
{
    GtkWidget *widget = GTK_WIDGET(fixture->display);
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = gtk_native_get_surface(native);
    graphene_point_t point = GRAPHENE_POINT_INIT(x, y);
    graphene_point_t native_point;
    Window child;
    int root_x, root_y;
    int scale = gdk_surface_get_scale_factor(surface);
    double tx, ty;

    g_assert_true(gtk_widget_compute_point(widget, GTK_WIDGET(native),
                                           &point, &native_point));
    gtk_native_get_surface_transform(native, &tx, &ty);
    g_assert_true(XTranslateCoordinates(fixture->xdisplay,
                                        gdk_x11_surface_get_xid(surface),
                                        DefaultRootWindow(fixture->xdisplay),
                                        0, 0, &root_x, &root_y, &child));
    g_assert_true(XTestFakeMotionEvent(fixture->xdisplay, -1,
                                      root_x + lround((native_point.x - tx) * scale),
                                      root_y + lround((native_point.y - ty) * scale),
                                      CurrentTime));
    XSync(fixture->xdisplay, False);
}

static void button(InputFixture *fixture, guint number, gboolean press, guint mask)
{
    guint events = fixture->button_events + 1;
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

    g_assert_true(XTestFakeButtonEvent(fixture->xdisplay, number, press, CurrentTime));
    XSync(fixture->xdisplay, False);
    while (fixture->button_events < events) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
    g_assert_cmpuint(fixture->button_events, ==, events);
    g_assert_cmpuint(fixture->display->priv->mouse_button_mask, ==, mask);
}

static void nested_chord(InputFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    GtkWidget *widget = GTK_WIDGET(fixture->display);

    /* The small centered image and nonzero ancestor/sibling offsets make a
     * surface-coordinate/widget-coordinate mixup reject these valid presses. */
    move_pointer(fixture, gtk_widget_get_width(widget) / 2.0,
                  gtk_widget_get_height(widget) / 2.0);
    button(fixture, 1, TRUE, SPICE_MOUSE_BUTTON_MASK_LEFT);
    button(fixture, 3, TRUE, SPICE_MOUSE_BUTTON_MASK_LEFT | SPICE_MOUSE_BUTTON_MASK_RIGHT);
    button(fixture, 3, FALSE, SPICE_MOUSE_BUTTON_MASK_LEFT);
    button(fixture, 1, FALSE, 0);
}

static void drag_release(InputFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    GtkWidget *widget = GTK_WIDGET(fixture->display);

    move_pointer(fixture, gtk_widget_get_width(widget) / 2.0,
                  gtk_widget_get_height(widget) / 2.0);
    button(fixture, 1, TRUE, SPICE_MOUSE_BUTTON_MASK_LEFT);
    /* Leave the guest image but stay within the widget: the forwarded press
     * must still be balanced, while a new press in the margin is ignored. */
    move_pointer(fixture, 5, 5);
    button(fixture, 3, TRUE, SPICE_MOUSE_BUTTON_MASK_LEFT);
    button(fixture, 3, FALSE, SPICE_MOUSE_BUTTON_MASK_LEFT);
    button(fixture, 1, FALSE, 0);
    button(fixture, 1, TRUE, 0);
    button(fixture, 1, FALSE, 0);
}

int main(int argc, char **argv)
{
    const char *display = g_getenv("SPICE_TEST_DISPLAY");
    Display *xdisplay;
    int event, error, major, minor;

    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_test_init(&argc, &argv, NULL);
    /* Never inject input into an ambient desktop. As for the video test, the
     * caller must explicitly provide a disposable, private display server. */
    if (display == NULL || display[0] != ':') {
        g_print("SKIP: set SPICE_TEST_DISPLAY to a private X11 display\n");
        return 77;
    }
    g_setenv("GDK_BACKEND", "x11", TRUE);
    g_setenv("DISPLAY", display, TRUE);
    g_assert_true(gtk_init_check());
    xdisplay = gdk_x11_display_get_xdisplay(gdk_display_get_default());
    if (!XTestQueryExtension(xdisplay, &event, &error, &major, &minor)) {
        g_print("SKIP: XTest extension unavailable\n");
        return 77;
    }
    g_test_add("/display/input/nested-chord", InputFixture, NULL,
               input_setup, nested_chord, input_teardown);
    g_test_add("/display/input/drag-release-outside-image", InputFixture, NULL,
               input_setup, drag_release, input_teardown);
    return g_test_run();
}
