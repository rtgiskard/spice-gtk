spice-gtk
=========

A Gtk client and libraries for SPICE remote desktop servers.

Please report bugs at: spice-devel@lists.freedesktop.org

Project content
---------------

* **libspice-client-glib-2.0**

    Provides glib objects for spice protocol decoding and surface rendering.
    * SpiceSession (see spice-session.h).
    * SpiceChannel (see spice-channel.h).
    * SpiceAudio (see spice-audio.h).
    * Various Spice\<Type\>Channel (see channel-\<type\>.h).

* **libspice-client-gtk-4.0**

    Provides the GTK 4 display widget and input integration in `src/gtk4/`.
    * SpiceDisplay (see spice-widget.h).
    * Uses GDK's floating-point surface scale for fractional HiDPI output;
      rendering, input coordinates, cursors and guest resize follow output changes.
    * The display's `scaling` property controls guest-image fitting, independently
      of application UI scaling. With fitting disabled, guest pixels target 1:1
      physical output pixels; Cairo uses nearest-neighbor sampling at integer image scales.
    * Guest resizing needs QXL, spice-vdagent and a desktop display component that
      applies DRM/KMS hot-plug changes, such as GNOME/Mutter. A bare Xorg server
      or a desktop that keeps the current mode when the preferred mode changes
      will not automatically apply the request; see the
      [SPICE guest implementation notes](https://www.spice-space.org/multiple-monitors.html#guest).
    * Visible DMA-BUF frames use GPU-native GTK textures without application-side
      CPU readback or upload. `GL_DRAW_DONE` is sent when the widget constructs its
      snapshot, not when GPU consumption finishes. For performance, this accepts
      possible frame overwrites or tearing if the producer reuses shared storage
      while GTK still references it. Descriptor lifetime does not freeze contents.
      Pending draws are acknowledged on hide, scanout reset and empty snapshots;
      hidden displays acknowledge new draws without importing a texture.
    * Full-surface streaming video uses `gtk4paintablesink` in the existing GTK
      canvas snapshot, preserving monitor crop, scaling, cursors and composition
      with sibling widgets. GL/DMA-BUF negotiation avoids the framebuffer copy
      where supported; screenshots read back only on request. Views of one stream
      share its paintable, including views added during playback. Removing a view
      does not stop the decoder-owned pipeline. Streams that cannot use the native
      sink retain the existing appsink/framebuffer renderer.
    * Wayland keyboard grabs use GDK's native shortcut inhibition. `keyboard-grab`
      follows compositor-confirmed state; pending requests are also released on
      focus loss, hide and teardown, and focused widgets recover after remapping.
    * Clipboard ownership is per SPICE session, including other local application
      providers. URI transfers preserve KDE cut metadata with asynchronous reads,
      owner-change cancellation and a cumulative clipboard-size bound.

* **spicy-test-gtk4**

    Development smoke client built with the library (not installed).

* **spicy-screenshot**

    Command line tool that connects to a SPICE server and writes a screenshot.

* **spicy-stats**

    Command line tool that summarizes a SPICE connection.

* **SpiceClientGlib** and **SpiceClientGtk** GObject-introspection modules.

Building
--------

Basic build (GTK 4 client library):

    meson setup build
    meson compile -C build

The GTK 3 widget, GIR, pkg-config file and `spicy` client are no longer built.
Applications must use `spice-client-gtk-4.0` and `SpiceClientGtk-4.0`.

Mouse-input regressions build when GTK's X11 backend and the XTest development
library are available. They inject real input and require an explicitly selected
private X11 server; never use the desktop's display. For example:

    SPICE_TEST_DISPLAY=:1 GSETTINGS_BACKEND=memory meson test -C build test-widget-input

Use the private server's actual display name, and repeat with `GDK_SCALE=2` to
check scaled, nested-widget input coordinates.

Rendering regressions need a native GTK display. Clipboard regressions skip unless
`SPICE_TEST_DISPLAY` names an already-running private Wayland compositor; never
point that test at the desktop clipboard. Run the suite on that private display:

    export SPICE_TEST_DISPLAY=wayland-test
    WAYLAND_DISPLAY="$SPICE_TEST_DISPLAY" GDK_BACKEND=wayland \
      GSETTINGS_BACKEND=memory GTK_A11Y=none meson test -C build --print-errorlogs

Replace `wayland-test` with the private compositor's actual socket name.

The video regression also requires `SPICE_TEST_DISPLAY` and the GTK 4 GStreamer
plugin. To exercise only video on a private X11 display, set it to that display's
name (for example `:1`) and run `meson test -C build test-video-rendering`.

Build dependencies:
------------------

Install GTK 4.14 or later development headers and introspection tools (when building GIR).
For example, on Fedora:

>>>
    dnf install gtk4-devel gobject-introspection-devel
>>>

Other dependencies include meson, ninja, spice-protocol, OpenSSL,
pixman, GStreamer, libjpeg, zlib and python3-pyparsing.

The GStreamer backend needs:

>>>
    gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good gstreamer1-plugins-bad-free
>>>

Install the GStreamer GTK 4 plugin (`gst-plugin-gtk4` on Arch Linux) and the
OpenGL plugin (`glsinkbin`) for native video presentation. The GTK 4 plugin must
be built with the appropriate Wayland/X11 GL and DMA-BUF features for GPU-native
frames. Missing plugins fall back to the framebuffer renderer; negotiated caps,
not just the sink name, determine whether frames remain GPU-native.
