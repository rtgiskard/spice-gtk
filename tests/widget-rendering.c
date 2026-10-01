#include "spice-widget-priv.h"

static gboolean have_display;

typedef struct {
    SpiceSession *session;
    SpiceSession *channel_session;
    SpiceDisplay *display;
} Fixture;

static void fixture_setup(Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    if (!have_display) {
        g_test_skip("No native GTK display available");
        return;
    }

    fixture->session = spice_session_new();
    fixture->channel_session = spice_session_new();
    fixture->display = spice_display_new(fixture->session, 0);
    g_object_ref_sink(fixture->display);
    /* Keep the real channels disconnected: the widget's session deliberately
     * has no channels, so constructing the fixture cannot initiate a socket. */
    fixture->display->priv->display = SPICE_DISPLAY_CHANNEL(
        spice_channel_new(fixture->channel_session, SPICE_CHANNEL_DISPLAY, 0));
}

static void fixture_teardown(Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    g_clear_object(&fixture->display);
    g_clear_object(&fixture->channel_session);
    g_clear_object(&fixture->session);
}

#ifdef HAVE_EGL
static const guint8 rgba[] = {
    255, 0, 0, 255,   0, 255, 0, 255,
    0, 0, 255, 255,   255, 255, 0, 255,
};

static void set_frame(Fixture *fixture, gboolean y0top,
                      int x, int y, int width, int height)
{
    SpiceDisplayPrivate *d = fixture->display->priv;
    GBytes *bytes = g_bytes_new_static(rgba, sizeof(rgba));

    d->dmabuf.enabled = TRUE;
    d->dmabuf.scanout.y0top = y0top;
    d->dmabuf.scanout_texture = gdk_memory_texture_new(2, 2,
                                                      GDK_MEMORY_R8G8B8A8,
                                                      bytes, 8);
    g_bytes_unref(bytes);
    d->area = (GdkRectangle) { x, y, width, height };
}

static void assert_pixel(GdkPixbuf *pixbuf, int x, int y,
                          guint8 red, guint8 green, guint8 blue)
{
    const guint8 *pixel = gdk_pixbuf_read_pixels(pixbuf) +
                         y * gdk_pixbuf_get_rowstride(pixbuf) +
                         x * gdk_pixbuf_get_n_channels(pixbuf);

    g_assert_cmpint(pixel[0], ==, red);
    g_assert_cmpint(pixel[1], ==, green);
    g_assert_cmpint(pixel[2], ==, blue);
}

static void screenshot_full(Fixture *fixture, gconstpointer data)
{
    gboolean y0top = GPOINTER_TO_INT(data);
    GdkPixbuf *pixbuf;

    if (fixture->display == NULL)
        return;
    set_frame(fixture, y0top, 0, 0, 2, 2);
    pixbuf = spice_display_get_pixbuf(fixture->display);
    g_assert_nonnull(pixbuf);
    g_assert_cmpint(gdk_pixbuf_get_width(pixbuf), ==, 2);
    g_assert_cmpint(gdk_pixbuf_get_height(pixbuf), ==, 2);
    assert_pixel(pixbuf, 0, y0top ? 0 : 1, 255, 0, 0);
    assert_pixel(pixbuf, 1, y0top ? 0 : 1, 0, 255, 0);
    assert_pixel(pixbuf, 0, y0top ? 1 : 0, 0, 0, 255);
    assert_pixel(pixbuf, 1, y0top ? 1 : 0, 255, 255, 0);
    g_object_unref(pixbuf);
}

static void screenshot_crop(Fixture *fixture, gconstpointer data)
{
    gboolean y0top = GPOINTER_TO_INT(data);
    GdkPixbuf *pixbuf;

    if (fixture->display == NULL)
        return;
    set_frame(fixture, y0top, 1, 1, 1, 1);
    pixbuf = spice_display_get_pixbuf(fixture->display);
    g_assert_nonnull(pixbuf);
    g_assert_cmpint(gdk_pixbuf_get_width(pixbuf), ==, 1);
    g_assert_cmpint(gdk_pixbuf_get_height(pixbuf), ==, 1);
    assert_pixel(pixbuf, 0, 0, y0top ? 255 : 0, 255, 0);
    g_object_unref(pixbuf);
}

static void screenshot_invalid_crop(Fixture *fixture,
                                     gconstpointer data G_GNUC_UNUSED)
{
    if (fixture->display == NULL)
        return;
    set_frame(fixture, FALSE, 1, 1, 2, 1);
    g_assert_null(spice_display_get_pixbuf(fixture->display));
    fixture->display->priv->area = (GdkRectangle) { 0, 2, 1, 1 };
    g_assert_null(spice_display_get_pixbuf(fixture->display));
}
#endif

static gboolean emit_scroll(Fixture *fixture, double dy)
{
    gboolean handled = FALSE;

    g_signal_emit_by_name(fixture->display->priv->scroll_controller,
                          "scroll", 0.0, dy, &handled);
    return handled;
}

static void scroll_handling(Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    SpiceDisplayPrivate *d;

    if (fixture->display == NULL)
        return;
    d = fixture->display->priv;
    g_assert_false(emit_scroll(fixture, 0.25));
    g_assert_cmpfloat(d->scroll_delta_y, ==, 0.0);

    d->inputs = SPICE_INPUTS_CHANNEL(
        spice_channel_new(fixture->channel_session, SPICE_CHANNEL_INPUTS, 0));
    g_object_set(fixture->display, "disable-inputs", TRUE, NULL);
    g_assert_false(emit_scroll(fixture, 0.25));
    g_assert_cmpfloat(d->scroll_delta_y, ==, 0.0);

    g_object_set(fixture->display, "disable-inputs", FALSE, NULL);
    g_assert_true(emit_scroll(fixture, 0.25));
    g_assert_cmpfloat(d->scroll_delta_y, ==, 0.25);
    g_assert_true(emit_scroll(fixture, 0.5));
    g_assert_cmpfloat(d->scroll_delta_y, ==, 0.75);
    g_assert_true(emit_scroll(fixture, -0.5));
    g_assert_cmpfloat(d->scroll_delta_y, ==, 0.25);
}

static void focus_remap(Fixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    if (!have_display)
        return;

    GtkWidget *widget = GTK_WIDGET(fixture->display);
    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    g_object_set(widget, "grab-keyboard", FALSE, "grab-mouse", FALSE, NULL);
    gtk_window_set_child(window, widget);
    gtk_window_present(window);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!gtk_widget_get_mapped(widget)) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    gtk_widget_grab_focus(widget);
    g_assert_true(fixture->display->priv->keyboard_have_focus);
    gtk_widget_set_visible(widget, FALSE);
    g_assert_false(fixture->display->priv->keyboard_have_focus);
    gtk_widget_set_visible(widget, TRUE);
    g_assert_true(gtk_widget_is_focus(widget));
    g_assert_true(fixture->display->priv->keyboard_have_focus);
    gtk_window_set_child(window, NULL);
    gtk_window_destroy(window);
}

int main(int argc, char **argv)
{
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_test_init(&argc, &argv, NULL);
    have_display = gtk_init_check();

#ifdef HAVE_EGL
    g_test_add("/display/screenshot/top-origin", Fixture, GINT_TO_POINTER(TRUE),
               fixture_setup, screenshot_full, fixture_teardown);
    g_test_add("/display/screenshot/bottom-origin", Fixture, GINT_TO_POINTER(FALSE),
               fixture_setup, screenshot_full, fixture_teardown);
    g_test_add("/display/screenshot/crop-top-origin", Fixture, GINT_TO_POINTER(TRUE),
               fixture_setup, screenshot_crop, fixture_teardown);
    g_test_add("/display/screenshot/crop-bottom-origin", Fixture, GINT_TO_POINTER(FALSE),
               fixture_setup, screenshot_crop, fixture_teardown);
    g_test_add("/display/screenshot/invalid-crop", Fixture, NULL,
               fixture_setup, screenshot_invalid_crop, fixture_teardown);
#endif
    g_test_add("/display/scroll/handled-and-accumulated", Fixture, NULL,
               fixture_setup, scroll_handling, fixture_teardown);
    g_test_add("/display/focus/remap", Fixture, NULL,
               fixture_setup, focus_remap, fixture_teardown);
    return g_test_run();
}
