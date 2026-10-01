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

#include <errno.h>
#include <unistd.h>
#include <gdk/gdk.h>

#include "spice-widget.h"
#include "spice-widget-priv.h"


typedef struct {
    guint n_planes;
    int fd[4];
} ScanoutFds;

static void scanout_fds_free(gpointer data)
{
    ScanoutFds *fds = data;
    guint i;

    for (i = 0; i < fds->n_planes; i++)
        close(fds->fd[i]);
    g_free(fds);
}

G_GNUC_INTERNAL
void spice_dmabuf_clear_scanout(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    DISPLAY_DEBUG(display, "clear GTK4 dmabuf scanout");

    g_clear_object(&d->dmabuf.scanout_texture);
    d->dmabuf.scanout.fd[0] = -1;
}

G_GNUC_INTERNAL
gboolean spice_dmabuf_update_scanout(SpiceDisplay *display,
                                    const SpiceGlScanout2 *scanout,
                                    GError **err)
{
    SpiceDisplayPrivate *d = display->priv;
    GdkDmabufTextureBuilder *builder;
    GdkDisplay *gdk_dpy;
    ScanoutFds *fds;
    GdkTexture *texture;
    guint j;

    g_return_val_if_fail(scanout != NULL, FALSE);
    if (scanout->num_planes > 4 ||
        (scanout->fd[0] >= 0 && scanout->num_planes == 0)) {
        g_set_error_literal(err, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                            "Invalid dmabuf plane count");
        return FALSE;
    }

    DISPLAY_DEBUG(display, "GTK4 dmabuf update: fd[0]:%d stride[0]:%u y0:%d %ux%u "
                  "format:0x%x (%c%c%c%c) modifier:0x%" G_GINT64_MODIFIER "x",
                  scanout->fd[0], scanout->stride[0], scanout->y0top,
                  scanout->width, scanout->height, scanout->format,
                  (int)scanout->format & 0xff,
                  (int)(scanout->format >> 8) & 0xff,
                  (int)(scanout->format >> 16) & 0xff,
                  (int)(scanout->format >> 24) & 0xff,
                  scanout->modifier);

    d->dmabuf.scanout = *scanout;

    if (scanout->fd[0] == -1) {
        g_clear_object(&d->dmabuf.scanout_texture);
        return TRUE;
    }

    gdk_dpy = gtk_widget_get_display(GTK_WIDGET(display));
    /* Keep duplicated descriptors alive for the retained GPU texture. */
    fds = g_new0(ScanoutFds, 1);
    for (j = 0; j < scanout->num_planes; j++) {
        int fd = scanout->fd[j] >= 0 ? scanout->fd[j] : scanout->fd[0];
        fds->fd[j] = dup(fd);
        if (fds->fd[j] < 0) {
            g_set_error(err, G_FILE_ERROR, g_file_error_from_errno(errno),
                        "Cannot duplicate dmabuf fd: %s", g_strerror(errno));
            scanout_fds_free(fds);
            return FALSE;
        }
        fds->n_planes++;
    }
    builder = gdk_dmabuf_texture_builder_new();

    gdk_dmabuf_texture_builder_set_display(builder, gdk_dpy);
    gdk_dmabuf_texture_builder_set_width(builder, scanout->width);
    gdk_dmabuf_texture_builder_set_height(builder, scanout->height);
    gdk_dmabuf_texture_builder_set_fourcc(builder, scanout->format);
    gdk_dmabuf_texture_builder_set_modifier(builder, scanout->modifier);
    gdk_dmabuf_texture_builder_set_n_planes(builder, scanout->num_planes);
    gdk_dmabuf_texture_builder_set_premultiplied(builder, TRUE);

    for (j = 0; j < scanout->num_planes; j++) {
        gdk_dmabuf_texture_builder_set_fd(builder, j, fds->fd[j]);
        gdk_dmabuf_texture_builder_set_stride(builder, j, scanout->stride[j]);
        gdk_dmabuf_texture_builder_set_offset(builder, j, scanout->offset[j]);
    }

    texture = gdk_dmabuf_texture_builder_build(builder,
                                                scanout_fds_free, fds, err);
    g_object_unref(builder);

    if (texture == NULL) {
        scanout_fds_free(fds);
        g_prefix_error(err, "Failed to create dmabuf texture: ");
        return FALSE;
    }
    g_set_object(&d->dmabuf.scanout_texture, texture);
    g_object_unref(texture);


    DISPLAY_DEBUG(display, "GTK4 dmabuf texture created: %ux%u",
                  gdk_texture_get_width(d->dmabuf.scanout_texture),
                  gdk_texture_get_height(d->dmabuf.scanout_texture));

    return TRUE;
}
