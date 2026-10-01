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
#pragma once

#include "config.h"
#include <math.h>
#include <gst/gst.h>

#ifdef WIN32
#include <windows.h>
#endif


#include "spice-widget.h"
#include "spice-common.h"
#include "spice-gtk-session.h"

/* GTK4 note: GdkPoint was removed in GTK4.  We use SpicePoint from
 * spice-common/common/draw.h (int32_t x, y) which is already pulled
 * in via spice-common.h above. */


G_BEGIN_DECLS

#define DISPLAY_DEBUG(display, fmt, ...) \
    SPICE_DEBUG("%d:%d " fmt, \
                SPICE_DISPLAY(display)->priv->channel_id, \
                SPICE_DISPLAY(display)->priv->monitor_id, \
                ## __VA_ARGS__)

typedef struct _SpiceDisplayPrivate SpiceDisplayPrivate;

struct _SpiceDisplay {
    GtkBox parent;
    SpiceDisplayPrivate *priv;
};

struct _SpiceDisplayClass {
    GtkBoxClass parent_class;

    /* signals */
    void (*mouse_grab)(SpiceChannel *channel, gint grabbed);
    void (*keyboard_grab)(SpiceChannel *channel, gint grabbed);
};

struct _SpiceDisplayPrivate {
    GtkStack                *stack;
    GtkWidget               *label;
    gint                    channel_id;
    gint                    monitor_id;

    /* options */
    bool                    keyboard_grab_enable;
    gboolean                keyboard_grab_inhibit;
    bool                    mouse_grab_enable;
    bool                    resize_guest_enable;

    /* state */
    gboolean                ready;
    gboolean                monitor_ready;
    struct {
        enum SpiceSurfaceFmt    format;
        gint                    width, height, stride;
        gpointer                data_origin; /* the original display image data */
        gpointer                data; /* converted if necessary to 32 bits */
        bool                    convert;
        cairo_surface_t         *surface;
    } canvas;
    struct {
        GstElement          *pipeline;
        GdkPaintable        *paintable;
    } video;
    GdkRectangle            area;
    /* window border */
    gint                    ww, wh, mx, my;

    gboolean                allow_scaling;
    gboolean                only_downscale;
    gboolean                disable_inputs;

    SpiceSession            *session;
    SpiceGtkSession         *gtk_session;
    SpiceMainChannel        *main;
    SpiceDisplayChannel     *display;
    SpiceCursorChannel      *cursor;
    SpiceInputsChannel      *inputs;
    SpiceSmartcardChannel   *smartcard;

    enum SpiceMouseMode     mouse_mode;
    int                     mouse_button_mask;
    int                     mouse_grab_active;
    bool                    mouse_have_pointer;
    GdkCursor               *mouse_cursor;
    GdkPixbuf               *mouse_pixbuf;
    SpicePoint              mouse_hotspot;
    GdkCursor               *show_cursor;
    int                     mouse_last_x;
    int                     mouse_last_y;
    int                     mouse_guest_x;
    int                     mouse_guest_y;
    cairo_surface_t         *cursor_surface;

    bool                    keyboard_grab_active;
    GdkToplevel             *shortcut_toplevel;
    gulong                  shortcut_notify_id;
    bool                    keyboard_have_focus;
    bool                    auto_usbredir_requested;

    const guint16          *keycode_map;
    size_t                  keycode_maplen;
    uint32_t                key_state[512 / 32];
    int                     key_delayed_scancode;
    guint                   key_delayed_id;
    SpiceGrabSequence         *grabseq; /* the configured key sequence */
    gboolean                *activeseq; /* the currently pressed keys */
    gboolean                seq_pressed;
    gboolean                keyboard_grab_released;
    gint                    mark;
#ifdef WIN32
    HHOOK                   keyboard_hook;
    int                     win_mouse[3];
    int                     win_mouse_speed;
#endif
    guint                   keypress_delay;
    gint                    zoom_level;
#ifdef GDK_WINDOWING_X11
    int                     x11_accel_numerator;
    int                     x11_accel_denominator;
    int                     x11_threshold;
#endif
#ifdef HAVE_EGL
    struct {
        gboolean            enabled;
        GdkTexture          *scanout_texture;
        guint               pending_draws;
        SpiceGlScanout2     scanout;
    } dmabuf;
#endif // HAVE_EGL
    double scroll_delta_y;
    /* Event controllers */
    GtkEventController      *key_controller;
    GtkEventController      *motion_controller;
    GtkEventController      *scroll_controller;
    GtkEventController      *button_controller;
};

/* Round once where logical widget units become device pixels. */
static inline gint spice_display_physical_size(gint logical, double scale)
{
    return lround(logical * scale);
}

double   spice_display_get_surface_scale       (SpiceDisplay *display);

int      spice_cairo_image_create                 (SpiceDisplay *display);
void     spice_cairo_image_destroy                (SpiceDisplay *display);
void     spice_cairo_draw_event                   (SpiceDisplay *display, cairo_t *cr);
void     spice_cairo_draw_cursor                  (SpiceDisplay *display, cairo_t *cr);
gboolean spice_allow_scaling                      (SpiceDisplay *display);
void     spice_display_get_scaling           (SpiceDisplay *display, double *s, int *x, int *y, int *w, int *h);
void     spice_dmabuf_clear_scanout          (SpiceDisplay *display);
gboolean spice_dmabuf_update_scanout         (SpiceDisplay *display,
                                             const SpiceGlScanout2 *scanout,
                                             GError **err);

#ifdef HAVE_EGL
void     spice_display_widget_gl_scanout     (SpiceDisplay *display);
#endif
void     spice_display_widget_update_monitor_area(SpiceDisplay *display);

G_END_DECLS
