#include "config.h"
#include <gtk/gtk.h>
#include <glib/gstdio.h>

/* Exercise private state without adding installed test-only API. */
#define PROP_0 PORT_PROP_0
#include "../src/channel-port.c"
#undef PROP_0

static guint notifications;
static GBytes *notification;
static void capture_notify(SpiceMainChannel *main, guint selection, guint32 type,
                           const guchar *data, size_t size);
#define spice_main_channel_clipboard_selection_notify capture_notify
#include "../src/gtk4/spice-gtk-session.c"
#undef spice_main_channel_clipboard_selection_notify

static void capture_notify(SpiceMainChannel *main G_GNUC_UNUSED,
                           guint selection, guint32 type,
                           const guchar *data, size_t size)
{
    g_assert_cmpuint(selection, ==, VD_AGENT_CLIPBOARD_SELECTION_CLIPBOARD);
    g_assert_cmpuint(type, ==, VD_AGENT_CLIPBOARD_FILE_LIST);
    notifications++;
    g_clear_pointer(&notification, g_bytes_unref);
    notification = g_bytes_new(data, size);
}

typedef struct {
    SpiceSession *session;
    SpiceGtkSession *gtk;
    SpiceMainChannel *main;
} Fixture;

static Fixture fixture_new(void)
{
    Fixture f = { 0 };
    f.session = spice_session_new();
    /* Never use the user's default shared directory. */
    g_object_set(f.session, "shared-dir", NULL, NULL);
    f.gtk = g_object_new(SPICE_TYPE_GTK_SESSION, "session", f.session,
                         "auto-clipboard", FALSE, NULL);
    f.main = SPICE_MAIN_CHANNEL(spice_channel_new(f.session, SPICE_CHANNEL_MAIN, 0));
    g_assert_true(f.gtk->priv->main == f.main);
    return f;
}

static void fixture_clear(Fixture *f)
{
    g_object_unref(f->gtk);
    g_object_unref(f->session);
}

static GdkContentProvider *install_guest(Fixture *f)
{
    guint index = 0;
    while (atom2agent[index].vdagent != VD_AGENT_CLIPBOARD_UTF8_TEXT)
        index++;
    GdkContentProvider *provider = spice_content_provider_new(f->gtk, 0, &index, 1);
    g_assert_true(gdk_clipboard_set_content(f->gtk->priv->clipboard, provider));
    g_weak_ref_set(&f->gtk->priv->guest_provider[0], provider);
    g_assert_true(clipboard_is_owned(f->gtk, f->gtk->priv->clipboard, 0));
    return provider;
}

static void host_text(void)
{
    Fixture f = fixture_new();
    GdkClipboard *cb = f.gtk->priv->clipboard;
    GdkContentProvider *guest = install_guest(&f);
    GdkContentProvider *host = gdk_content_provider_new_typed(G_TYPE_STRING, "local entry text");
    g_assert_true(gdk_clipboard_set_content(cb, host));
    g_assert_false(f.gtk->priv->clipboard_by_guest[0]);
    clipboard_get_targets_gtk4(f.gtk, cb);
    g_assert_true(f.gtk->priv->clip_grabbed[0]);
    g_assert_false(clipboard_is_owned(f.gtk, cb, 0));
    clipboard_release(f.gtk, 0);
    g_assert_true(gdk_clipboard_get_content(cb) == host);
    channel_destroy(f.session, SPICE_CHANNEL(f.main), f.gtk);
    g_assert_true(gdk_clipboard_get_content(cb) == host);
    fixture_clear(&f);
    g_assert_true(gdk_clipboard_get_content(cb) == host);
    gdk_clipboard_set_content(cb, NULL);
    g_object_unref(host);
    g_object_unref(guest);
}

static void session_replacement(void)
{
    Fixture a = fixture_new();
    Fixture b = fixture_new();
    GdkClipboard *cb = a.gtk->priv->clipboard;
    GdkContentProvider *old = install_guest(&a);
    GdkContentProvider *current = install_guest(&b);
    g_assert_false(a.gtk->priv->clipboard_by_guest[0]);
    g_assert_true(b.gtk->priv->clipboard_by_guest[0]);
    clipboard_release(a.gtk, 0);
    g_assert_true(gdk_clipboard_get_content(cb) == current);
    channel_destroy(a.session, SPICE_CHANNEL(a.main), a.gtk);
    g_assert_true(gdk_clipboard_get_content(cb) == current);
    fixture_clear(&a);
    g_assert_true(gdk_clipboard_get_content(cb) == current);
    fixture_clear(&b);
    /* The provider is kept alive here deliberately: session weak refs have
     * disappeared, but final disposal must still clear its own content. */
    g_assert_null(gdk_clipboard_get_content(cb));
    g_object_unref(old);
    g_object_unref(current);
}

#ifdef HAVE_PHODAV_VIRTUAL
static gboolean wait_for_read(Fixture *f, gboolean metadata)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        GList *pending = f->gtk->priv->pending_reads;
        if (!metadata && pending == NULL)
            return TRUE;
        if (metadata && pending != NULL && ((ClipboardRead *)pending->data)->uri_bytes != NULL)
            return TRUE;
        if (metadata && pending == NULL)
            return FALSE;
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    return FALSE;
}

static void kde_files(gconstpointer variant)
{
    const char *mode = variant;
    Fixture f = fixture_new();
    GError *error = NULL;
    gchar *dir = g_dir_make_tmp("spice-gtk-clipboard-XXXXXX", &error);
    g_assert_no_error(error);
    g_object_set(f.session, "shared-dir", dir, NULL);
    SpiceChannel *webdav = spice_channel_new(f.session, SPICE_CHANNEL_WEBDAV, 0);
    SPICE_PORT_CHANNEL(webdav)->priv->opened = TRUE;
    GFile *file = g_file_new_for_path(dir);
    gchar *uri = g_file_get_uri(file);
    gchar *uri_list = g_strdup_printf("# ignored comment\r\n%s\r\n", uri);
    /* Real conversion, but no filesystem publication: use an already-shared
     * private fixture mapping, as repeated consumer requests normally do. */
    g_hash_table_insert(f.gtk->priv->cb_shared_files, file,
                        g_strdup("/clipboard/test-file"));
    GBytes *uris = g_bytes_new(uri_list, strlen(uri_list));
    GBytes *flag = g_bytes_new(strcmp(mode, "copy") == 0 ? "0" : "1", 1);
    GdkContentProvider *parts[] = {
        gdk_content_provider_new_for_bytes("text/uri-list", uris),
        gdk_content_provider_new_for_bytes("application/x-kde-cutselection", flag),
    };
    GdkContentProvider *provider = gdk_content_provider_new_union(parts, G_N_ELEMENTS(parts));
    GdkClipboard *cb = f.gtk->priv->clipboard;
    g_assert_true(gdk_clipboard_set_content(cb, provider));
    clipboard_get_targets_gtk4(f.gtk, cb);
    g_assert_true(f.gtk->priv->clip_grabbed[0]);
    if (strcmp(mode, "limit") == 0)
        g_object_set(f.main, "max-clipboard", (gint)strlen(uri_list), NULL);
    notifications = 0;
    g_clear_pointer(&notification, g_bytes_unref);
    g_assert_true(clipboard_request(f.main, 0, VD_AGENT_CLIPBOARD_FILE_LIST, f.gtk));
    if (strcmp(mode, "cancel") == 0) {
        g_assert_true(wait_for_read(&f, TRUE));
        ClipboardRead *read = f.gtk->priv->pending_reads->data;
        g_assert_cmpstr((const char *)read->uri_bytes->data, ==, uri_list);
        GCancellable *cancel = g_object_ref(read->cancel);
        GdkContentProvider *replacement = gdk_content_provider_new_typed(G_TYPE_STRING, "replacement");
        g_assert_true(gdk_clipboard_set_content(cb, replacement));
        g_assert_true(g_cancellable_is_cancelled(cancel));
        /* Cancellation removes the pending list entry before the callback.
         * The request's cancellable dies only once that callback is consumed. */
        GWeakRef completed;
        g_weak_ref_init(&completed, cancel);
        g_object_unref(cancel);
        gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        for (;;) {
            GObject *alive = g_weak_ref_get(&completed);
            if (alive == NULL)
                break;
            g_object_unref(alive);
            g_assert_cmpint(g_get_monotonic_time(), <, deadline);
            g_main_context_iteration(NULL, FALSE);
            g_usleep(1000);
        }
        g_weak_ref_clear(&completed);
        g_assert_cmpuint(notifications, ==, 0);
        g_assert_true(gdk_clipboard_get_content(cb) == replacement);
        g_object_unref(replacement);
    } else {
        g_assert_true(wait_for_read(&f, FALSE));
        g_assert_cmpuint(notifications, ==, 1);
        if (strcmp(mode, "limit") == 0) {
            g_assert_cmpuint(g_bytes_get_size(notification), ==, 0);
        } else {
            const char *verb = strcmp(mode, "copy") == 0 ? "copy" : "cut";
            gsize size;
            const char *payload = g_bytes_get_data(notification, &size);
            gchar *expected = g_strdup_printf("%s%c/clipboard/test-file", verb, '\0');
            /* g_strdup_printf retains the embedded NUL; protocol length
             * includes both the action and filename terminators. */
            gsize expected_size = strlen(verb) + 1 + strlen("/clipboard/test-file") + 1;
            g_assert_cmpmem(payload, size, expected, expected_size);
            g_free(expected);
        }
    }
    gdk_clipboard_set_content(cb, NULL);
    g_object_unref(provider);
    g_bytes_unref(flag);
    g_bytes_unref(uris);
    fixture_clear(&f);
    g_assert_cmpint(g_rmdir(dir), ==, 0);
    g_free(dir);
    g_free(uri);
    g_free(uri_list);
    g_clear_pointer(&notification, g_bytes_unref);
}
#endif

int main(int argc, char **argv)
{
    const char *display = g_getenv("SPICE_TEST_DISPLAY");
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
    g_test_init(&argc, &argv, NULL);
    /* An ordinary full-suite invocation must not touch the desktop clipboard. */
    if (display == NULL || *display == '\0') {
        g_print("SKIP: set SPICE_TEST_DISPLAY to a private Wayland compositor\n");
        return 77;
    }
    g_setenv("GDK_BACKEND", "wayland", TRUE);
    g_setenv("WAYLAND_DISPLAY", display, TRUE);
    g_assert_true(gtk_init_check());
    g_test_add_func("/gtk-clipboard/local-string", host_text);
    g_test_add_func("/gtk-clipboard/session-replacement", session_replacement);
#ifdef HAVE_PHODAV_VIRTUAL
    g_test_add_data_func("/gtk-clipboard/kde/cut", "cut", kde_files);
    g_test_add_data_func("/gtk-clipboard/kde/copy", "copy", kde_files);
    g_test_add_data_func("/gtk-clipboard/kde/cancel", "cancel", kde_files);
    g_test_add_data_func("/gtk-clipboard/kde/limit", "limit", kde_files);
#endif
    return g_test_run();
}
