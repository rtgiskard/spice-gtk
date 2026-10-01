Title: Overview
Slug: overview

# SpiceClientGtk Overview

SpiceClientGtk provides GTK widgets for embedding a SPICE display
in a GTK application.

## Display Widget

The [class@SpiceClientGtk.Display] widget renders the guest framebuffer
and handles keyboard and mouse input. It supports both software rendering
and OpenGL acceleration.

```c
SpiceDisplay *display = spice_display_new(session, 0);
gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(display));
```

### Keyboard and Mouse Grabbing

The display widget can grab keyboard and mouse input to provide a
seamless guest interaction experience. Grabbing behavior is controlled
by the [property@SpiceClientGtk.Display:grab-keyboard] and
[property@SpiceClientGtk.Display:grab-mouse] properties. A custom
key sequence can be configured to release grabs using
[method@SpiceClientGtk.Display.set_grab_keys].

### Scaling and Zoom

The widget supports display scaling (controlled by
[property@SpiceClientGtk.Display:scaling]) and zoom level adjustment
([property@SpiceClientGtk.Display:zoom-level]).

## GTK Session

[class@SpiceClientGtk.GtkSession] provides GTK integration for clipboard
sharing and automatic USB redirection, bridging the GTK clipboard with
the SPICE agent clipboard.

## USB Device Widget

[class@SpiceClientGtk.UsbDeviceWidget] is a ready-made widget that
lists available USB devices with connect/disconnect toggle buttons.
