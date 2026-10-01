#include "config.h"
#include <stdio.h>
#include "spice-client.h"
#include "spice-channel-priv.h"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include "channel-display-priv.h"

/* Use the production callbacks without exposing test-only installed API. */
#include "../src/gtk4/spice-widget.c"
#include "../src/channel-display-gst.c"

#define GUEST_WIDTH 192
#define GUEST_HEIGHT 128
#define PIXEL_TOLERANCE 3

typedef struct {
    GstElement *generator;
    GstElement *source;
    GstElement *samples;
    GstElement *playbin;
    gint frames;
} Video;

typedef struct {
    SpiceSession *session;
    SpiceSession *channel_session;
    SpiceDisplayChannel *channel;
    SpiceDisplay *display;
    GtkWindow *window;
    display_surface surface;
    display_stream stream;
    guint32 canvas[GUEST_WIDTH * GUEST_HEIGHT];
    Video videos[2];
} VideoFixture;

static void iterate_display(void)
{
    while (g_main_context_iteration(NULL, FALSE))
        ;
    g_usleep(1000);
}

static void assert_pipeline_ok(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);

    if (message != NULL) {
        GError *error = NULL;
        char *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        g_error("GStreamer error: %s (%s)", error->message, debug ? debug : "");
    }
    gst_object_unref(bus);
}

static void feed_video(GstAppSrc *appsrc, guint length G_GNUC_UNUSED,
                       gpointer user_data)
{
    Video *video = user_data;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(video->samples),
                                                    GST_SECOND);
    if (sample != NULL) {
        GstFlowReturn flow = gst_app_src_push_sample(appsrc, sample);
        if (flow == GST_FLOW_OK)
            g_atomic_int_inc(&video->frames);
        gst_sample_unref(sample);
    }
}

static void configure_source(GstElement *playbin G_GNUC_UNUSED,
                             GstElement *source, gpointer user_data)
{
    Video *video = user_data;
    GstCaps *caps = NULL;

    g_object_get(video->samples, "caps", &caps, NULL);
    gst_app_src_set_caps(GST_APP_SRC(source), caps);
    gst_caps_unref(caps);
    g_object_set(source, "format", GST_FORMAT_TIME, "is-live", TRUE, NULL);
    g_signal_connect(source, "need-data", G_CALLBACK(feed_video), video);
}

static void video_clear(Video *video)
{
    if (video->playbin != NULL) {
        gst_element_set_state(video->playbin, GST_STATE_NULL);
        gst_clear_object(&video->playbin);
    }
    if (video->generator != NULL) {
        gst_element_set_state(video->generator, GST_STATE_NULL);
        gst_clear_object(&video->source);
        gst_clear_object(&video->samples);
        gst_clear_object(&video->generator);
    }
}

static gboolean install_video(VideoFixture *fixture, GstElement *pipeline)
{
    return hand_pipeline_to_widget(&fixture->stream,
                                   pipeline != NULL ? GST_PIPELINE(pipeline) : NULL);
}

static void video_start(VideoFixture *fixture, Video *video, const char *pattern,
                        guint32 color, int width, int height)
{
    GError *error = NULL;
    char *description = g_strdup_printf(
        "videotestsrc name=source is-live=true pattern=%s foreground-color=%u "
        "! video/x-raw,format=RGB,width=%d,height=%d,framerate=30/1 "
        "! appsink name=samples caps=video/x-raw,format=RGB,width=%d,height=%d,framerate=30/1 "
        "sync=false max-buffers=2 drop=true", pattern, color,
        width, height, width, height);

    video->generator = gst_parse_launch(description, &error);
    g_free(description);
    g_assert_no_error(error);
    g_assert_nonnull(video->generator);
    video->source = gst_bin_get_by_name(GST_BIN(video->generator), "source");
    video->samples = gst_bin_get_by_name(GST_BIN(video->generator), "samples");
    video->playbin = gst_element_factory_make("playbin", NULL);
    g_assert_nonnull(video->playbin);
    /* This is the real decoder-to-widget handoff, not a substitute renderer. */
    g_assert_true(install_video(fixture, video->playbin));
    g_object_set(video->playbin, "uri", "appsrc://", "flags", 1, NULL);
    g_signal_connect(video->playbin, "source-setup", G_CALLBACK(configure_source), video);
    g_assert_cmpint(gst_element_set_state(video->generator, GST_STATE_PLAYING),
                    !=, GST_STATE_CHANGE_FAILURE);
    g_assert_cmpint(gst_element_set_state(video->playbin, GST_STATE_PLAYING),
                    !=, GST_STATE_CHANGE_FAILURE);
}

static void check_videos(VideoFixture *fixture)
{
    for (guint i = 0; i < G_N_ELEMENTS(fixture->videos); i++) {
        if (fixture->videos[i].generator != NULL)
            assert_pipeline_ok(fixture->videos[i].generator);
        if (fixture->videos[i].playbin != NULL)
            assert_pipeline_ok(fixture->videos[i].playbin);
    }
}

static void create_canvas(VideoFixture *fixture)
{
    for (guint i = 0; i < G_N_ELEMENTS(fixture->canvas); i++)
        fixture->canvas[i] = 0x000000ff;
    primary_create(SPICE_CHANNEL(fixture->channel), SPICE_SURFACE_FMT_32_xRGB,
                   GUEST_WIDTH, GUEST_HEIGHT, GUEST_WIDTH * 4, -1,
                   fixture->canvas, fixture->display);
    mark(fixture->display, TRUE);
}

static void video_view_setup(VideoFixture *fixture)
{
    fixture->display = spice_display_new(fixture->session, 0);
    g_object_ref_sink(fixture->display);
    fixture->display->priv->mouse_mode = SPICE_MOUSE_MODE_CLIENT;
    g_object_set(spice_gtk_session_get(fixture->session), "auto-clipboard", FALSE, NULL);
    g_object_set(fixture->display, "grab-keyboard", FALSE, "grab-mouse", FALSE,
                 "disable-inputs", TRUE, "scaling", TRUE, NULL);
    fixture->display->priv->display = fixture->channel;
    fixture->surface.streaming_mode = TRUE;
    fixture->stream.surface = &fixture->surface;
    fixture->stream.channel = SPICE_CHANNEL(fixture->channel);
    g_signal_connect_object(fixture->channel, "gst-video-overlay",
                            G_CALLBACK(set_overlay), fixture->display, G_CONNECT_AFTER);
    create_canvas(fixture);
    fixture->window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_default_size(fixture->window, GUEST_WIDTH * 2, GUEST_HEIGHT * 2);
    gtk_window_set_child(fixture->window, GTK_WIDGET(fixture->display));
    gtk_window_present(fixture->window);
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!gtk_widget_get_mapped(GTK_WIDGET(fixture->display))) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
}

static void video_fixture_setup(VideoFixture *fixture,
                                gconstpointer data G_GNUC_UNUSED)
{
    fixture->session = spice_session_new();
    g_object_set(fixture->session, "shared-dir", NULL, NULL);
    fixture->channel_session = spice_session_new();
    /* The channel belongs to another session, so no channel-new handler can
     * auto-connect a real socket. Only the real rendering signal is attached. */
    fixture->channel = SPICE_DISPLAY_CHANNEL(spice_channel_new(
        fixture->channel_session, SPICE_CHANNEL_DISPLAY, 0));
    video_view_setup(fixture);
}

static void video_fixture_teardown(VideoFixture *fixture,
                                   gconstpointer data G_GNUC_UNUSED)
{
    for (guint i = 0; i < G_N_ELEMENTS(fixture->videos); i++)
        video_clear(&fixture->videos[i]);
    if (fixture->window != NULL) {
        gtk_window_set_child(fixture->window, NULL);
        gtk_window_destroy(fixture->window);
    }
    g_clear_object(&fixture->display);
    g_clear_object(&fixture->channel_session);
    g_clear_object(&fixture->session);
}

static gboolean pixel_matches(GdkPixbuf *pixbuf, int x, int y,
                               guint8 red, guint8 green, guint8 blue)
{
    const guint8 *pixel = gdk_pixbuf_read_pixels(pixbuf) +
        y * gdk_pixbuf_get_rowstride(pixbuf) + x * gdk_pixbuf_get_n_channels(pixbuf);
    return ABS((int)pixel[0] - red) <= PIXEL_TOLERANCE &&
           ABS((int)pixel[1] - green) <= PIXEL_TOLERANCE &&
           ABS((int)pixel[2] - blue) <= PIXEL_TOLERANCE;
}

static gboolean solid_matches(GdkPixbuf *pixbuf, guint8 red, guint8 green, guint8 blue)
{
    if (pixbuf == NULL)
        return FALSE;
    for (int y = 0; y < gdk_pixbuf_get_height(pixbuf); y++) {
        for (int x = 0; x < gdk_pixbuf_get_width(pixbuf); x++) {
            if (!pixel_matches(pixbuf, x, y, red, green, blue))
                return FALSE;
        }
    }
    return TRUE;
}

static GdkPixbuf *window_pixels(VideoFixture *fixture)
{
    GtkWidget *widget = GTK_WIDGET(fixture->display);
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GskRenderNode *node;
    GdkPixbuf *pixbuf = NULL;

    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    node = gtk_snapshot_free_to_node(snapshot);
    if (node != NULL) {
        graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(fixture->window));
        GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &bounds);
        GdkTextureDownloader *downloader = gdk_texture_downloader_new(texture);
        gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
        gsize stride;
        GBytes *bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
        pixbuf = gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB, TRUE, 8,
                                           gdk_texture_get_width(texture),
                                           gdk_texture_get_height(texture), stride);
        g_bytes_unref(bytes);
        gdk_texture_downloader_free(downloader);
        g_object_unref(texture);
        gsk_render_node_unref(node);
    }
    g_object_unref(paintable);
    return pixbuf;
}

static void wait_solid(VideoFixture *fixture, int width, int height,
                       guint8 red, guint8 green, guint8 blue)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    for (;;) {
        check_videos(fixture);
        GdkPixbuf *pixbuf = spice_display_get_pixbuf(fixture->display);
        gboolean matches = solid_matches(pixbuf, red, green, blue);
        if (matches) {
            g_assert_cmpint(gdk_pixbuf_get_width(pixbuf), ==, width);
            g_assert_cmpint(gdk_pixbuf_get_height(pixbuf), ==, height);
        }
        g_clear_object(&pixbuf);
        if (matches)
            return;
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
}

static void wait_visible_solid(VideoFixture *fixture,
                               guint8 red, guint8 green, guint8 blue)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    for (;;) {
        check_videos(fixture);
        GdkPixbuf *pixbuf = window_pixels(fixture);
        gboolean matches = pixbuf != NULL;
        int left, top, width, height;
        double surface_scale = spice_display_get_surface_scale(fixture->display);
        spice_display_get_scaling(fixture->display, NULL, &left, &top, &width, &height);
        /* The compositor may allocate a tall window: sample the fitted video,
         * not fractions of the enclosing window's letterbox. */
        for (int y = 1; matches && y <= 3; y++) {
            for (int x = 1; matches && x <= 3; x++)
                matches = pixel_matches(pixbuf, (left + x * width / 4) / surface_scale,
                                         (top + y * height / 4) / surface_scale,
                                         red, green, blue);
        }
        g_clear_object(&pixbuf);
        if (matches)
            return;
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
}

static void wait_frames(VideoFixture *fixture, Video *video, int count)
{
    int target = g_atomic_int_get(&video->frames) + count;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_atomic_int_get(&video->frames) < target) {
        check_videos(fixture);
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
}

static void video_frames_and_replacement(VideoFixture *fixture,
                                         gconstpointer data G_GNUC_UNUSED)
{
    Video *first = &fixture->videos[0];
    Video *second = &fixture->videos[1];
    /* Decoded dimensions differ from both guest and allocated widget size. */
    video_start(fixture, first, "solid-color", 0xffff0000, 96, 64);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    g_object_set(first->source, "foreground-color", 0xff00ff00, NULL);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 0, 255, 0);
    wait_visible_solid(fixture, 0, 255, 0);

    video_start(fixture, second, "solid-color", 0xffff0000, 96, 64);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    /* A detached sink keeps receiving frames until its decoder stops it. */
    g_object_set(first->source, "foreground-color", 0xffffff00, NULL);
    wait_frames(fixture, first, 3);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    release_pipeline_from_widget(&fixture->stream, GST_PIPELINE(first->playbin));
    wait_frames(fixture, second, 3);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    release_pipeline_from_widget(&fixture->stream, GST_PIPELINE(second->playbin));
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 0, 0, 255);
    wait_visible_solid(fixture, 0, 0, 255);
}

static void video_crop(VideoFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    Video *video = &fixture->videos[0];
    const int x = 19, y = 13, width = 73, height = 45;
    GdkPixbuf *full = NULL;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    video_start(fixture, video, "checkers-8", 0xffff0000, GUEST_WIDTH, GUEST_HEIGHT);
    /* Checkers are nonuniform: comparing the crop to the complete decoded
     * image detects ignoring either crop origin, not just wrong dimensions. */
    for (;;) {
        check_videos(fixture);
        full = spice_display_get_pixbuf(fixture->display);
        if (full != NULL && !solid_matches(full, 0, 0, 255))
            break;
        g_clear_object(&full);
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
    g_assert_cmpint(gdk_pixbuf_get_width(full), ==, GUEST_WIDTH);
    g_assert_cmpint(gdk_pixbuf_get_height(full), ==, GUEST_HEIGHT);
    g_assert_true(pixel_matches(full, 4, 4, 255, 0, 0) ||
                  pixel_matches(full, 4, 4, 0, 255, 0));
    const guint8 *checker = gdk_pixbuf_read_pixels(full) +
        4 * gdk_pixbuf_get_rowstride(full) + 4 * gdk_pixbuf_get_n_channels(full);
    g_assert_false(pixel_matches(full, 12, 4, checker[0], checker[1], checker[2]));
    update_area(fixture->display, x, y, width, height);
    GdkPixbuf *crop = spice_display_get_pixbuf(fixture->display);
    g_assert_nonnull(crop);
    g_assert_cmpint(gdk_pixbuf_get_width(crop), ==, width);
    g_assert_cmpint(gdk_pixbuf_get_height(crop), ==, height);
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            const guint8 *pixel = gdk_pixbuf_read_pixels(full) +
                (row + y) * gdk_pixbuf_get_rowstride(full) +
                (col + x) * gdk_pixbuf_get_n_channels(full);
            g_assert_true(pixel_matches(crop, col, row, pixel[0], pixel[1], pixel[2]));
        }
    }
    g_object_unref(crop);
    g_object_unref(full);
}

static void video_software_return(VideoFixture *fixture, gconstpointer data)
{
    Video *video = &fixture->videos[0];
    video_start(fixture, video, "solid-color", 0xffff0000, 96, 64);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    if (GPOINTER_TO_INT(data)) {
        primary_destroy(fixture->channel, fixture->display);
        create_canvas(fixture);
    } else {
        g_assert_true(install_video(fixture, NULL));
    }
    /* Continuing decoded frames must not replace the restored canvas. */
    wait_frames(fixture, video, 3);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 0, 0, 255);
    wait_visible_solid(fixture, 0, 0, 255);
}

static void video_dispose(VideoFixture *fixture, gconstpointer data G_GNUC_UNUSED)
{
    Video *video = &fixture->videos[0];
    gpointer weak_display = fixture->display;
    video_start(fixture, video, "solid-color", 0xffff0000, 96, 64);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    g_object_add_weak_pointer(G_OBJECT(fixture->display), &weak_display);
    gtk_window_set_child(fixture->window, NULL);
    g_clear_object(&fixture->display);
    g_assert_null(weak_display);
    /* Exercise actual streaming and main-context invalidations after final
     * widget destruction, rather than asserting only disconnected IDs. */
    g_object_set(video->source, "foreground-color", 0xff00ff00, NULL);
    wait_frames(fixture, video, 5);
    g_assert_false(install_video(fixture, NULL));
}

static void video_shared_views(VideoFixture *fixture, gconstpointer data)
{
    VideoFixture other = { 0 };
    Video *video = &fixture->videos[0];
    gboolean late = GPOINTER_TO_INT(data);
    gpointer weak_display = fixture->display;

    other.session = g_object_ref(fixture->session);
    other.channel_session = g_object_ref(fixture->channel_session);
    other.channel = fixture->channel;
    if (!late)
        video_view_setup(&other);
    video_start(fixture, video, "solid-color", 0xffff0000, 96, 64);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    if (late) {
        video_view_setup(&other);
        g_signal_handlers_disconnect_by_data(other.channel, other.display);
        /* Exercise the real channel-new replay, without connecting a socket.
         * The primary's pixels were installed by video_view_setup(). */
        SPICE_CHANNEL(other.channel)->priv->state = SPICE_CHANNEL_STATE_READY;
        channel_new(other.session, SPICE_CHANNEL(other.channel), other.display);
        SPICE_CHANNEL(other.channel)->priv->state = SPICE_CHANNEL_STATE_UNCONNECTED;
    }
    wait_solid(&other, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    wait_visible_solid(&other, 255, 0, 0);
    g_object_set(video->source, "foreground-color", 0xff00ff00, NULL);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 0, 255, 0);
    wait_solid(&other, GUEST_WIDTH, GUEST_HEIGHT, 0, 255, 0);

    g_object_add_weak_pointer(G_OBJECT(fixture->display), &weak_display);
    gtk_window_set_child(fixture->window, NULL);
    g_clear_object(&fixture->display);
    g_assert_null(weak_display);
    g_object_set(video->source, "foreground-color", 0xffffff00, NULL);
    wait_solid(&other, GUEST_WIDTH, GUEST_HEIGHT, 255, 255, 0);
    wait_visible_solid(&other, 255, 255, 0);
    video_fixture_teardown(&other, NULL);
    wait_frames(fixture, video, 5);
}

static void queue_jpeg(VideoFixture *fixture, SpiceGstDecoder *decoder,
                       const char *jpeg, gsize size)
{
    SpiceMsgIn *message = spice_msg_in_new(SPICE_CHANNEL(fixture->channel));
    SpiceFrame *frame = g_new0(SpiceFrame, 1);

    message->data = g_memdup2(jpeg, size);
    frame->data = message->data;
    frame->data_opaque = message;
    frame->size = size;
    frame->mm_time = stream_get_time(&fixture->stream);
    frame->creation_time = g_get_monotonic_time();
    frame->dest = (SpiceRect) { 0, 0, GUEST_WIDTH, GUEST_HEIGHT };
    g_assert_true(spice_gst_decoder_queue_frame(&decoder->base, frame, 50));
}

static void wait_decoder_drained(SpiceGstDecoder *decoder)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;

    for (;;) {
        guint queued;
        guint64 bytes;
        g_mutex_lock(&decoder->queues_mutex);
        queued = decoder->decoding_queue->length;
        g_mutex_unlock(&decoder->queues_mutex);
        g_object_get(decoder->appsrc, "current-level-bytes", &bytes, NULL);
        if (queued == 0 && bytes == 0)
            break;
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        iterate_display();
    }
    GstState state;
    gst_element_get_state(decoder->pipeline, &state, NULL, 0);
    g_assert_cmpint(state, ==, GST_STATE_PLAYING);
}

static void video_decoder_after_dispose(VideoFixture *fixture,
                                        gconstpointer data G_GNUC_UNUSED)
{
    SpiceGstDecoder *decoder = g_new0(SpiceGstDecoder, 1);
    GdkPixbuf *image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 96, 64);
    char *jpeg = NULL;
    gsize size = 0;
    GError *error = NULL;
    gpointer weak_display = fixture->display;

    /* Select the real playbin decoder path explicitly so this regression
     * does not depend on the test machine's hardware decoder availability. */
    decoder->base.codec_type = SPICE_VIDEO_CODEC_TYPE_MJPEG;
    decoder->base.stream = &fixture->stream;
    decoder->last_mm_time = stream_get_time(&fixture->stream);
    decoder->decoding_queue = g_queue_new();
    g_mutex_init(&decoder->queues_mutex);
    g_assert_true(create_pipeline(decoder, false));
    g_assert_null(decoder->appsink);
    g_assert_nonnull(decoder->appsrc);

    gdk_pixbuf_fill(image, 0xff0000ff);
    g_assert_true(gdk_pixbuf_save_to_buffer(image, &jpeg, &size, "jpeg", &error,
                                          "quality", "100", NULL));
    g_assert_no_error(error);
    queue_jpeg(fixture, decoder, jpeg, size);
    wait_solid(fixture, GUEST_WIDTH, GUEST_HEIGHT, 255, 0, 0);
    wait_visible_solid(fixture, 255, 0, 0);
    wait_decoder_drained(decoder);

    g_object_add_weak_pointer(G_OBJECT(fixture->display), &weak_display);
    gtk_window_set_child(fixture->window, NULL);
    g_clear_object(&fixture->display);
    g_assert_null(weak_display);
    /* A stopped pipeline leaves appsrc non-NULL, so queue_frame keeps
     * accepting encoded frames. Pace frames instead of forcing QoS drops
     * with a burst of already-late timestamps; each must reach the sink. */
    for (guint i = 0; i < 10; i++) {
        queue_jpeg(fixture, decoder, jpeg, size);
        wait_decoder_drained(decoder);
    }
    spice_gst_decoder_destroy(&decoder->base);
    g_assert_null(g_object_get_data(G_OBJECT(fixture->channel), SPICE_DISPLAY_NATIVE_PIPELINE));
    g_free(jpeg);
    g_object_unref(image);
}

int main(int argc, char **argv)
{
    const char *display = g_getenv("SPICE_TEST_DISPLAY");
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_setenv("GTK_A11Y", "none", TRUE);
    g_test_init(&argc, &argv, NULL);
    if (display == NULL || *display == '\0') {
        g_print("SKIP: set SPICE_TEST_DISPLAY to a private Wayland or X11 display\n");
        return 77;
    }
    if (display[0] == ':') {
        g_setenv("GDK_BACKEND", "x11", TRUE);
        g_setenv("DISPLAY", display, TRUE);
    } else {
        g_setenv("GDK_BACKEND", "wayland", TRUE);
        g_setenv("WAYLAND_DISPLAY", display, TRUE);
    }
    g_assert_true(gtk_init_check());
    gst_init(&argc, &argv);
    GstElementFactory *factory = gst_element_factory_find("gtk4paintablesink");
    if (factory == NULL) {
        g_print("SKIP: optional gtk4paintablesink plugin is unavailable\n");
        return 77;
    }
    gst_object_unref(factory);
    g_test_add("/display/video/frames-and-replacement", VideoFixture, NULL,
               video_fixture_setup, video_frames_and_replacement, video_fixture_teardown);
    g_test_add("/display/video/monitor-crop", VideoFixture, NULL,
               video_fixture_setup, video_crop, video_fixture_teardown);
    g_test_add("/display/video/null-restores-software", VideoFixture, GINT_TO_POINTER(FALSE),
               video_fixture_setup, video_software_return, video_fixture_teardown);
    g_test_add("/display/video/primary-reset-restores-software", VideoFixture, GINT_TO_POINTER(TRUE),
               video_fixture_setup, video_software_return, video_fixture_teardown);
    g_test_add("/display/video/dispose-while-streaming", VideoFixture, NULL,
               video_fixture_setup, video_dispose, video_fixture_teardown);
    g_test_add("/display/video/shared-channel", VideoFixture, GINT_TO_POINTER(FALSE),
               video_fixture_setup, video_shared_views, video_fixture_teardown);
    g_test_add("/display/video/late-shared-channel", VideoFixture, GINT_TO_POINTER(TRUE),
               video_fixture_setup, video_shared_views, video_fixture_teardown);
    g_test_add("/display/video/decoder-after-dispose", VideoFixture, NULL,
               video_fixture_setup, video_decoder_after_dispose, video_fixture_teardown);
    return g_test_run();
}
