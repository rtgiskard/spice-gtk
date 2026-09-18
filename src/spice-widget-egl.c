/*
   Copyright (C) 2014-2016 Red Hat, Inc.

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

#include <math.h>
#include <gdk/gdk.h>

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES

#include "spice-widget.h"
#include "spice-widget-priv.h"
#include "spice-gtk-session-priv.h"

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_WIN32
#include <gdk/gdkwin32.h>
#endif

/* Upper bound for waiting on the scanout blit, 1 second in nanoseconds */
#define SPICE_EGL_FENCE_TIMEOUT_NS (G_GUINT64_CONSTANT(1000000000))

/* How long before the vblank the swap is issued, capped at an eighth of the
 * refresh period so it stays sane on fast displays */
#define SPICE_EGL_PRESENT_GUARD_US (G_GINT64_CONSTANT(4000))

#define VERTS_ARRAY_SIZE (sizeof(GLfloat) * 4 * 4)
#define TEX_ARRAY_SIZE (sizeof(GLfloat) * 4 * 2)

static const char *spice_egl_vertex_src =       \
"                                               \
  #version 130\n                                \
                                                \
  in vec4 position;                             \
  in vec2 texcoords;                            \
  out vec2 tcoords;                             \
  uniform mat4 mproj;                           \
                                                \
  void main()                                   \
  {                                             \
    tcoords = texcoords;                        \
    gl_Position = mproj * position;             \
  }                                             \
";

static const char *spice_egl_fragment_src =     \
"                                               \
  #version 130\n                                \
                                                \
  in vec2 tcoords;                              \
  out vec4 fragmentColor;                       \
  uniform sampler2D samp;                       \
                                                \
  void  main()                                  \
  {                                             \
    fragmentColor = texture2D(samp, tcoords);   \
  }                                             \
";

static void apply_ortho(guint mproj, float left, float right,
                        float bottom, float top, float _near, float _far)

{
    float a = 2.0f / (right - left);
    float b = 2.0f / (top - bottom);
    float c = -2.0f / (_far - _near);

    float tx = - (right + left) / (right - left);
    float ty = - (top + bottom) / (top - bottom);
    float tz = - (_far + _near) / (_far - _near);

    float ortho[16] = {
        a, 0, 0, 0,
        0, b, 0, 0,
        0, 0, c, 0,
        tx, ty, tz, 1
    };

    glUniformMatrix4fv(mproj, 1, GL_FALSE, &ortho[0]);
}

static gboolean spice_egl_init_shaders(SpiceDisplay *display, GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    GLuint fs = 0, vs = 0, buf;
    GLint status, tex_loc, prog;
    gboolean success = FALSE;
    gchar log[1000] = { 0, };
    GLsizei len;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);

    fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, (const char **)&spice_egl_fragment_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(fs, sizeof(log), &len, log);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "failed to compile fragment shader: %s", log);
        goto end;
    }

    vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, (const char **)&spice_egl_vertex_src, NULL);
    glCompileShader(vs);
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(vs, sizeof(log), &len, log);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "failed to compile vertex shader: %s", log);
        goto end;
    }

    d->egl.prog = glCreateProgram();
    glAttachShader(d->egl.prog, fs);
    glAttachShader(d->egl.prog, vs);
    glLinkProgram(d->egl.prog);
    glGetProgramiv(d->egl.prog, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramInfoLog(d->egl.prog, sizeof(log), &len, log);
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "error linking shaders: %s", log);
        goto end;
    }

    glUseProgram(d->egl.prog);
    glDetachShader(d->egl.prog, fs);
    glDetachShader(d->egl.prog, vs);

    d->egl.attr_pos = glGetAttribLocation(d->egl.prog, "position");
    g_assert(d->egl.attr_pos != -1);
    d->egl.attr_tex = glGetAttribLocation(d->egl.prog, "texcoords");
    g_assert(d->egl.attr_tex != -1);
    tex_loc = glGetUniformLocation(d->egl.prog, "samp");
    g_assert(tex_loc != -1);
    d->egl.mproj = glGetUniformLocation(d->egl.prog, "mproj");
    g_assert(d->egl.mproj != -1);

    glUniform1i(tex_loc, 0);

    /* we only use one VAO, so we always keep it bound */
    glGenVertexArrays(1, &buf);
    glBindVertexArray(buf);

    glGenBuffers(1, &buf);
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER,
                 VERTS_ARRAY_SIZE + TEX_ARRAY_SIZE,
                 NULL,
                 GL_STATIC_DRAW);
    d->egl.vbuf_id = buf;

    glGenTextures(1, &d->egl.tex_id);
    glGenTextures(1, &d->egl.tex_pointer_id);

    success = TRUE;

end:
    if (fs) {
        glDeleteShader(fs);
    }
    if (vs) {
        glDeleteShader(vs);
    }

    glUseProgram(prog);
    return success;
}

G_GNUC_INTERNAL
gboolean spice_egl_init(SpiceDisplay *display, GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    static const EGLint conf_att[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    static const EGLint ctx_att[] = {
#ifdef EGL_CONTEXT_MAJOR_VERSION
        EGL_CONTEXT_MAJOR_VERSION, 3,
#else
        EGL_CONTEXT_CLIENT_VERSION, 3,
#endif
        EGL_NONE
    };
    EGLBoolean b;
    EGLint major, minor, n;
    EGLNativeDisplayType dpy = 0;
    GdkDisplay *gdk_dpy = gdk_display_get_default();

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(gdk_dpy)) {
        d->egl.ctx = eglGetCurrentContext();
        dpy = (EGLNativeDisplayType)gdk_wayland_display_get_wl_display(gdk_dpy);
        d->egl.display = eglGetDisplay(dpy);
        goto end;
    }
#endif
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdk_dpy)) {
        dpy = (EGLNativeDisplayType)gdk_x11_display_get_xdisplay(gdk_dpy);
    }
#endif
#ifdef GDK_WINDOWING_WIN32
    if (GDK_IS_WIN32_DISPLAY(gdk_dpy)) {
        dpy = (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY; /* or perhaps wglGetCurrentDC? */
    }
#endif

    d->egl.display = eglGetDisplay(dpy);
    if (d->egl.display == EGL_NO_DISPLAY) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "failed to get EGL display");
        return FALSE;
    }

    if (!eglInitialize(d->egl.display, &major, &minor)) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "failed to init EGL display");
        return FALSE;
    }

    SPICE_DEBUG("EGL major/minor: %d.%d\n", major, minor);
    SPICE_DEBUG("EGL version: %s\n",
                eglQueryString(d->egl.display, EGL_VERSION));
    SPICE_DEBUG("EGL vendor: %s\n",
                eglQueryString(d->egl.display, EGL_VENDOR));
    SPICE_DEBUG("EGL extensions: %s\n",
                eglQueryString(d->egl.display, EGL_EXTENSIONS));

    b = eglBindAPI(EGL_OPENGL_API);
    if (!b) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "cannot bind OpenGL API");
        return FALSE;
    }

    b = eglChooseConfig(d->egl.display, conf_att, &d->egl.conf,
                        1, &n);

    if (!b || n != 1) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "cannot find suitable EGL config");
        return FALSE;
    }

    d->egl.ctx = eglCreateContext(d->egl.display,
                                  d->egl.conf,
                                  EGL_NO_CONTEXT,
                                  ctx_att);
    if (!d->egl.ctx) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "cannot create EGL context");
        return FALSE;
    }

    eglMakeCurrent(d->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   d->egl.ctx);

#ifdef GDK_WINDOWING_WAYLAND
end:
#endif

    if (!spice_egl_init_shaders(display, err))
        return FALSE;

    d->egl.context_ready = TRUE;

    if (d->display != NULL &&
        spice_display_channel_get_gl_scanout2(d->display) != NULL) {
        DISPLAY_DEBUG(display, "scanout present during egl init, updating widget");
        spice_display_widget_gl_scanout(display);
        spice_display_widget_update_monitor_area(display);
    }

    return TRUE;
}

G_GNUC_INTERNAL
void spice_egl_set_x11_window_visual(SpiceDisplay *display, GtkWidget *widget)
{
#ifdef GDK_WINDOWING_X11
    SpiceDisplayPrivate *d = display->priv;
    EGLint visual_id = 0;
    GdkVisual *visual;

    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default()) ||
        d->egl.display == EGL_NO_DISPLAY ||
        d->egl.conf == NULL) {
        return;
    }

    if (gtk_widget_get_realized(widget)) {
        return;
    }

    if (eglGetConfigAttrib(d->egl.display, d->egl.conf,
                           EGL_NATIVE_VISUAL_ID, &visual_id) != EGL_TRUE ||
        visual_id == 0) {
        return;
    }

    visual = gdk_x11_screen_lookup_visual(gtk_widget_get_screen(widget),
                                          visual_id);
    if (visual == NULL) {
        g_warning("No GDK visual found for EGL native visual 0x%x", visual_id);
        return;
    }

    gtk_widget_set_visual(widget, visual);
#endif
}

static gboolean
gl_make_current(SpiceDisplay *display, GError **err)
{
    SpiceDisplayPrivate *d = display->priv;

    g_return_val_if_fail(d->egl.context_ready, FALSE);

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        EGLBoolean success = eglMakeCurrent(d->egl.display,
                                            d->egl.surface,
                                            d->egl.surface,
                                            d->egl.ctx);
        if (success != EGL_TRUE) {
            g_set_error_literal(err, SPICE_CLIENT_ERROR,
                                SPICE_CLIENT_ERROR_FAILED,
                                "failed to activate context");
            return FALSE;
        }
        return TRUE;
    }
#endif

    GtkWidget *area = gtk_stack_get_child_by_name(d->stack, "gl-area");

    gtk_gl_area_make_current(GTK_GL_AREA(area));

    return TRUE;
}

static void
spice_egl_prepare_default_framebuffer(void)
{
#ifdef GDK_WINDOWING_X11
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        return;
    }

    glDrawBuffer(GL_BACK);
    glReadBuffer(GL_BACK);
#endif
}

static gboolean spice_widget_init_egl_win(SpiceDisplay *display, GdkWindow *win,
                                          GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    EGLNativeWindowType native = 0;

    if (d->egl.surface)
        return TRUE;

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_WINDOW(win)) {
        if (!gdk_window_ensure_native(win)) {
            g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                                "failed to ensure native X11 window for EGL");
            return FALSE;
        }
        native = (EGLNativeWindowType)GDK_WINDOW_XID(win);
    }
#endif

    if (!native) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "this platform isn't supported");
        return FALSE;
    }

    d->egl.surface = eglCreateWindowSurface(d->egl.display,
                                            d->egl.conf,
                                            native, NULL);

    if (!d->egl.surface) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "failed to init egl surface: egl_error=0x%x",
                    eglGetError());
        return FALSE;
    }

    if (!gl_make_current(display, err))
        return FALSE;

    return TRUE;
}

G_GNUC_INTERNAL
gboolean spice_egl_realize_display(SpiceDisplay *display, GdkWindow *win, GError **err)
{
    DISPLAY_DEBUG(display, "egl realize");
    if (!spice_widget_init_egl_win(display, win, err))
        return FALSE;
    gint scale_factor = gtk_widget_get_scale_factor(GTK_WIDGET(display));
    spice_egl_resize_display(display, gdk_window_get_width(win) * scale_factor,
                             gdk_window_get_height(win) * scale_factor);

    return TRUE;
}

G_GNUC_INTERNAL
void spice_egl_unrealize_display(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    DISPLAY_DEBUG(display, "egl unrealize %p", d->egl.surface);

    d->egl.present_pending = FALSE;
    d->egl.last_present = 0;
    if (d->egl.present_timer_id != 0) {
        g_source_remove(d->egl.present_timer_id);
        d->egl.present_timer_id = 0;
    }

    if (!gl_make_current(display, NULL))
        return;

    if (d->egl.image != NULL) {
        eglDestroyImageKHR(d->egl.display, d->egl.image);
        d->egl.image = NULL;
    }

    if (d->egl.tex_id) {
        glDeleteTextures(1, &d->egl.tex_id);
        d->egl.tex_id = 0;
    }

    if (d->egl.tex_pointer_id) {
        glDeleteTextures(1, &d->egl.tex_pointer_id);
        d->egl.tex_pointer_id = 0;
    }

    if (d->egl.vbuf_id) {
        glDeleteBuffers(1, &d->egl.vbuf_id);
        d->egl.vbuf_id = 0;
    }

    if (d->egl.prog) {
        glDeleteProgram(d->egl.prog);
        d->egl.prog = 0;
    }

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        /* egl.surface && egl.ctx are only created on x11, see
           spice_egl_init() */

        if (d->egl.surface != EGL_NO_SURFACE) {
            eglDestroySurface(d->egl.display, d->egl.surface);
            d->egl.surface = EGL_NO_SURFACE;
        }

        if (d->egl.ctx) {
            eglDestroyContext(d->egl.display, d->egl.ctx);
            d->egl.ctx = 0;
        }

        eglMakeCurrent(d->egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);

        /* do not call eglterminate() since egl may be used by
         * somebody else code */
    }
#endif
}

/* w and h should be adjusted to gdk scaling */
G_GNUC_INTERNAL
void spice_egl_resize_display(SpiceDisplay *display, int w, int h)
{
    SpiceDisplayPrivate *d = display->priv;
    int prog;

    if (!d->egl.context_ready || !gl_make_current(display, NULL))
        return;

    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);

    glUseProgram(d->egl.prog);
    apply_ortho(d->egl.mproj, 0, w, 0, h, -1, 1);
    glViewport(0, 0, w, h);

    if (d->ready)
        spice_egl_update_display(display);

    glUseProgram(prog);
}

static void
draw_rect_from_arrays(SpiceDisplay *display, const void *verts, const void *tex)
{
    SpiceDisplayPrivate *d = display->priv;

    glBindBuffer(GL_ARRAY_BUFFER, d->egl.vbuf_id);

    if (verts) {
        glEnableVertexAttribArray(d->egl.attr_pos);
        glBufferSubData(GL_ARRAY_BUFFER,
                        0,
                        VERTS_ARRAY_SIZE,
                        verts);
        glVertexAttribPointer(d->egl.attr_pos, 4, GL_FLOAT,
                              GL_FALSE, 0, 0);
    }
    if (tex) {
        glEnableVertexAttribArray(d->egl.attr_tex);
        glBufferSubData(GL_ARRAY_BUFFER,
                        VERTS_ARRAY_SIZE,
                        TEX_ARRAY_SIZE,
                        tex);
        glVertexAttribPointer(d->egl.attr_tex, 2, GL_FLOAT,
                              GL_FALSE, 0,
                              (void *)VERTS_ARRAY_SIZE);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (verts)
        glDisableVertexAttribArray(d->egl.attr_pos);
    if (tex)
        glDisableVertexAttribArray(d->egl.attr_tex);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static GLvoid
client_draw_rect_tex(SpiceDisplay *display,
                     float x, float y, float w, float h,
                     float tx, float ty, float tw, float th)
{
    GLfloat tex[4][2] = {
        { tx, ty },
        { tx + tw, ty },
        { tx, ty + th },
        { tx + tw, ty + th },
    };

    GLfloat verts[4][4] = {
        { x, y, 0.0, 1.0 },
        { x + w, y, 0.0, 1.0 },
        { x, y + h, 0.0, 1.0 },
        { x + w, y + h, 0.0, 1.0 },
    };

    draw_rect_from_arrays(display, verts, tex);
}

G_GNUC_INTERNAL
void spice_egl_cursor_set(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    GdkPixbuf *image = d->mouse_pixbuf;

    g_return_if_fail(d->egl.enabled);

    if (image == NULL)
        return;

    int width = gdk_pixbuf_get_width(image);
    int height = gdk_pixbuf_get_height(image);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->egl.tex_pointer_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE,
                 gdk_pixbuf_read_pixels(image));
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void spice_egl_set_filter_for_scale(double s)
{
    GLint filter = s == rint(s) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

G_GNUC_INTERNAL
gboolean spice_egl_draw_display(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    double s;
    int x, y, w, h;
    gdouble tx, ty, tw, th;
    int prog;

    g_return_val_if_fail(d->ready, FALSE);
    if (!gl_make_current(display, NULL))
        return FALSE;

    spice_egl_prepare_default_framebuffer();

    spice_display_get_scaling(display, &s, &x, &y, &w, &h);

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    tx = (gdouble) d->area.x / d->egl.scanout.width;
    ty = (gdouble) d->area.y / d->egl.scanout.height;
    tw = (gdouble) d->area.width / d->egl.scanout.width;
    th = (gdouble) d->area.height / d->egl.scanout.height;

    /* convert to opengl coordinates, 0 is bottom, 1 is top. ty should
     * be the bottom of the area, since th is upward */
    /* 1+---------------+ */
    /*  |               | */
    /*  |  ty  to  |th  | */
    /*  |  |   ->  |    | */
    /*  |  |th     ty   | */
    /*  |               | */
    /*  |               | */
    /*  +---------------+ */
    /* 0 */
    ty = 1 - (ty + th);


    /* if the scanout is inverted, then invert coordinates and direction too */
    if (!d->egl.scanout.y0top) {
        ty = 1 - ty;
        th = -1 * th;
    }
    DISPLAY_DEBUG(display, "update %f +%d+%d %dx%d +%f+%f %fx%f", s, x, y, w, h,
                  tx, ty, tw, th);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->egl.tex_id);
    spice_egl_set_filter_for_scale(s);

    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)d->egl.image);

    glDisable(GL_BLEND);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glUseProgram(d->egl.prog);
    client_draw_rect_tex(display, x, y, w, h,
                         tx, ty, tw, th);

    if (d->mouse_mode == SPICE_MOUSE_MODE_SERVER &&
        d->mouse_guest_x != -1 && d->mouse_guest_y != -1 &&
        !d->show_cursor &&
        spice_gtk_session_get_pointer_grabbed(d->gtk_session) &&
        d->mouse_pixbuf != NULL) {
        GdkPixbuf *image = d->mouse_pixbuf;
        int width = gdk_pixbuf_get_width(image);
        int height = gdk_pixbuf_get_height(image);
        width = ceil(width * s);
        height = ceil(height * s);

        glBindTexture(GL_TEXTURE_2D, d->egl.tex_pointer_id);
        spice_egl_set_filter_for_scale(s);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        client_draw_rect_tex(display,
                             x + (d->mouse_guest_x - d->mouse_hotspot.x) * s,
                             y + h - (d->mouse_guest_y - d->mouse_hotspot.y) * s,
                             width, -height,
                             0, 0, 1, 1);
    }

    glUseProgram(prog);

    return TRUE;
}

/* Block until the GPU is done sampling the guest scanout texture, so that the
 * buffer can be handed back to the guest for reuse. Unlike waiting for
 * eglSwapBuffers(), this does not wait for a vblank. */
G_GNUC_INTERNAL
void spice_egl_wait_draw_complete(SpiceDisplay *display)
{
    GLsync fence;

    if (!gl_make_current(display, NULL))
        return;

    if (epoxy_gl_version() < 32 && !epoxy_has_gl_extension("GL_ARB_sync")) {
        glFinish();
        return;
    }

    fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence == NULL) {
        glFinish();
        return;
    }

    glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                     SPICE_EGL_FENCE_TIMEOUT_NS);
    glDeleteSync(fence);
}

static void spice_egl_present_now(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    d->egl.present_pending = FALSE;

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
        /* gtk+ does the swap with gtkglarea */
        eglSwapBuffers(d->egl.display, d->egl.surface);
    }
#endif

    /* The swap returns once the frame has been presented, so this is the
     * most recent vblank as far as this surface is concerned */
    d->egl.last_present = g_get_monotonic_time();
}

/* Refresh period of the monitor showing the widget. The widget's frame clock
 * cannot be used for this: a compositor that does not implement the frame
 * synchronisation protocol leaves it free running with no timing information
 * at all, which on a 30Hz display means it reports nothing usable. */
static gint64 spice_egl_refresh_period(SpiceDisplay *display)
{
    GdkWindow *window;
    GdkMonitor *monitor;
    int rate;

    window = gtk_widget_get_window(GTK_WIDGET(display));
    if (window == NULL)
        return 0;

    monitor = gdk_display_get_monitor_at_window(gdk_window_get_display(window),
                                                window);
    rate = monitor != NULL ? gdk_monitor_get_refresh_rate(monitor) : 0;
    if (rate <= 0)
        return 0;

    /* milli-Hz to microseconds */
    return G_USEC_PER_SEC * G_GINT64_CONSTANT(1000) / rate;
}

static gboolean spice_egl_present_timer(gpointer data)
{
    SpiceDisplay *display = SPICE_DISPLAY(data);

    display->priv->egl.present_timer_id = 0;
    if (display->priv->egl.present_pending)
        spice_egl_present_now(display);

    return G_SOURCE_REMOVE;
}

/*
 * Present a drawn frame.
 *
 * eglSwapBuffers() waits for the vblank, and the wait happens on the main
 * loop, where it also stops the display channel from being serviced. With
 * spice GL scanout the guest is blocked until the client acknowledges each
 * frame, so a swap issued as soon as the frame is drawn costs the guest a
 * full refresh period on every frame and throttles it to a fraction of the
 * display refresh rate.
 *
 * Turning vsync off would fix the frame rate but tears, because the
 * compositor samples the same window while it is being drawn into. Instead
 * the swap is deferred until shortly before the next vblank is due, using
 * the previous swap as the phase reference: the wait inside the swap shrinks
 * to the guard interval, presentation stays synchronised, and the guest is
 * free to render its next frame in the meantime.
 *
 * If the refresh period is not known the frame is presented immediately,
 * which is the behaviour this replaces.
 */
G_GNUC_INTERNAL
void spice_egl_queue_present(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    gint64 period, guard, due, now;

    d->egl.present_pending = TRUE;

    if (d->egl.present_timer_id != 0)
        return;

    period = spice_egl_refresh_period(display);
    if (period <= 0) {
        spice_egl_present_now(display);
        return;
    }

    guard = MIN(period / 8, SPICE_EGL_PRESENT_GUARD_US);
    now = g_get_monotonic_time();
    due = d->egl.last_present + period - guard;

    if (d->egl.last_present == 0 || due <= now) {
        spice_egl_present_now(display);
        return;
    }

    d->egl.present_timer_id = g_timeout_add((due - now) / 1000,
                                            spice_egl_present_timer, display);
}

G_GNUC_INTERNAL
void spice_egl_update_display(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    /* A frame that is drawn and waiting to be presented does not need to be
     * drawn again, which matters on large displays */
    if (!d->egl.present_pending && !spice_egl_draw_display(display))
        return;

    spice_egl_queue_present(display);
}

G_GNUC_INTERNAL
gboolean spice_egl_update_scanout(SpiceDisplay *display,
                                  const SpiceGlScanout2 *scanout,
                                  GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    EGLint attrs[64];
    guint32 format;
    int i = 0, j;

    EGLint fd_attrs[] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    EGLint offset_attrs[] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    EGLint stride_attrs[] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    EGLint modifier_lo_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    EGLint modifier_hi_attrs[] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    g_return_val_if_fail(scanout != NULL, FALSE);
    g_return_val_if_fail(scanout->num_planes <= 4, FALSE);

    format = scanout->format;

    if (d->egl.image != NULL) {
        eglDestroyImageKHR(d->egl.display, d->egl.image);
        d->egl.image = NULL;
    }

    if (scanout->fd[0] == -1)
        return TRUE;

    attrs[i++] = EGL_WIDTH;
    attrs[i++] = scanout->width;
    attrs[i++] = EGL_HEIGHT;
    attrs[i++] = scanout->height;
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[i++] = format;

    for (j = 0; j < scanout->num_planes; j++) {
        attrs[i++] = fd_attrs[j];
        attrs[i++] = scanout->fd[j] >= 0 ? scanout->fd[j] : scanout->fd[0];
        attrs[i++] = stride_attrs[j];
        attrs[i++] = scanout->stride[j];
        attrs[i++] = offset_attrs[j];
        attrs[i++] = scanout->offset[j];
        attrs[i++] = modifier_lo_attrs[j];
        attrs[i++] = (scanout->modifier >>  0) & 0xffffffff;
        attrs[i++] = modifier_hi_attrs[j];
        attrs[i++] = (scanout->modifier >> 32) & 0xffffffff;
    }
    attrs[i++] = EGL_NONE;

    DISPLAY_DEBUG(display, "fd[0]:%d stride[0]:%u y0:%d %ux%u format:0x%x (%c%c%c%c)",
                  scanout->fd[0], scanout->stride[0], scanout->y0top,
                  scanout->width, scanout->height, format,
                  (int)format & 0xff, (int)(format >> 8) & 0xff,
                  (int)(format >> 16) & 0xff, (int)format >> 24);

    d->egl.image = eglCreateImageKHR(d->egl.display,
                                       EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT,
                                       (EGLClientBuffer)NULL,
                                       attrs);

    if (d->egl.image == EGL_NO_IMAGE_KHR) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "failed to create EGL image from DMA-BUF: egl_error=0x%x",
                    eglGetError());
        return FALSE;
    }

    if (!gl_make_current(display, err)) {
        eglDestroyImageKHR(d->egl.display, d->egl.image);
        d->egl.image = NULL;
        return FALSE;
    }

    /* Clear stale GL errors so the following check only covers the image bind. */
    while (glGetError() != GL_NO_ERROR) {
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, d->egl.tex_id);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)d->egl.image);
    GLenum gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        g_set_error(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                    "failed to bind EGL image to GL texture: gl_error=0x%x",
                    gl_error);
        eglDestroyImageKHR(d->egl.display, d->egl.image);
        d->egl.image = NULL;
        return FALSE;
    }

    d->egl.scanout = *scanout;

    return TRUE;
}
