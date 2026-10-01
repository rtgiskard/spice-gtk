/*
  Copyright (C) 2010 Red Hat, Inc.

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

#include "spice-widget.h"
#include "spice-widget-priv.h"
#include "spice-gtk-session-priv.h"


G_GNUC_INTERNAL
int spice_cairo_image_create(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    double scale;

    if (d->canvas.surface != NULL)
        return 0;

    if (d->canvas.format == SPICE_SURFACE_FMT_16_555 ||
        d->canvas.format == SPICE_SURFACE_FMT_16_565) {
        d->canvas.convert = TRUE;
        d->canvas.data = g_malloc0(d->area.width * d->area.height * 4);

        d->canvas.surface = cairo_image_surface_create_for_data
            (d->canvas.data, CAIRO_FORMAT_RGB24,
             d->area.width, d->area.height, d->area.width * 4);

    } else {
        d->canvas.convert = FALSE;

        d->canvas.surface = cairo_image_surface_create_for_data
            (d->canvas.data, CAIRO_FORMAT_RGB24,
             d->canvas.width, d->canvas.height, d->canvas.stride);
    }

    scale = spice_display_get_surface_scale(display);
    cairo_surface_set_device_scale(d->canvas.surface, scale, scale);

    return 0;
}

G_GNUC_INTERNAL
void spice_cairo_image_destroy(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;

    g_clear_pointer(&d->canvas.surface, cairo_surface_destroy);
    if (d->canvas.convert)
        g_clear_pointer(&d->canvas.data, g_free);
    d->canvas.convert = FALSE;
}

G_GNUC_INTERNAL
void spice_cairo_draw_event(SpiceDisplay *display, cairo_t *cr)
{
    SpiceDisplayPrivate *d = display->priv;
    double s;
    double scale;
    int x, y;
    int ww, wh;
    int w, h;

    scale = spice_display_get_surface_scale(display);
    spice_display_get_scaling(display, &s, &x, &y, &w, &h);

    ww = gtk_widget_get_width(GTK_WIDGET(display));
    wh = gtk_widget_get_height(GTK_WIDGET(display));

    /* GTK snapshots are buffered; paint the letterbox before the framebuffer. */
    cairo_rectangle(cr, 0, 0, ww, wh);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_fill(cr);

    /* Draw the display */
    if (d->canvas.surface) {
        /* Keep physical pixel positions exact at fractional surface scales. */
        cairo_translate(cr, x / scale, y / scale);
        cairo_rectangle(cr, 0, 0, w / scale, h / scale);
        cairo_scale(cr, s, s);
        if (!d->canvas.convert)
            cairo_translate(cr, -d->area.x, -d->area.y);
        cairo_set_source_surface(cr, d->canvas.surface, 0, 0);
        if (s >= 1.0 && s == floor(s))
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_fill(cr);

        spice_cairo_draw_cursor(display, cr);
    }
}

G_GNUC_INTERNAL
void spice_cairo_draw_cursor(SpiceDisplay *display, cairo_t *cr)
{
    SpiceDisplayPrivate *d = display->priv;
    double scale = spice_display_get_surface_scale(display);

    if (d->mouse_mode == SPICE_MOUSE_MODE_SERVER &&
        d->mouse_guest_x != -1 && d->mouse_guest_y != -1 &&
        !d->show_cursor &&
        spice_gtk_session_get_pointer_grabbed(d->gtk_session) &&
        d->cursor_surface != NULL) {
        cairo_set_source_surface(cr, d->cursor_surface,
                                 (double)(d->mouse_guest_x - d->mouse_hotspot.x) / scale,
                                 (double)(d->mouse_guest_y - d->mouse_hotspot.y) / scale);
        cairo_paint(cr);
    }
}

G_GNUC_INTERNAL
gboolean spice_allow_scaling(SpiceDisplay *display)
{
    SpiceDisplayPrivate *d = display->priv;
    return d->allow_scaling;
}
