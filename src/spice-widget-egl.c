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

    d->egl.ctx = eglGetCurrentContext();
    d->egl.display = eglGetCurrentDisplay();

    if (d->egl.ctx == EGL_NO_CONTEXT || d->egl.display == EGL_NO_DISPLAY) {
        g_set_error_literal(err, SPICE_CLIENT_ERROR, SPICE_CLIENT_ERROR_FAILED,
                            "no current EGL context/display; is GTK using its EGL GL backend?");
        return FALSE;
    }

    SPICE_DEBUG("EGL version: %s\n",
                eglQueryString(d->egl.display, EGL_VERSION));
    SPICE_DEBUG("EGL vendor: %s\n",
                eglQueryString(d->egl.display, EGL_VENDOR));
    SPICE_DEBUG("EGL extensions: %s\n",
                eglQueryString(d->egl.display, EGL_EXTENSIONS));

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

static gboolean
gl_make_current(SpiceDisplay *display, GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    GtkWidget *area;

    g_return_val_if_fail(d->egl.context_ready, FALSE);

    area = gtk_stack_get_child_by_name(d->stack, "gl-area");
    gtk_gl_area_make_current(GTK_GL_AREA(area));

    return TRUE;
}

G_GNUC_INTERNAL
void spice_egl_unrealize_display(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    DISPLAY_DEBUG(display, "egl unrealize");

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
void spice_egl_update_display(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    double s;
    int x, y, w, h;
    gdouble tx, ty, tw, th;
    int prog;

    g_return_if_fail(d->ready);
    if (!gl_make_current(display, NULL))
        return;

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
