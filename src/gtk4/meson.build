#
# Source files for spice-client-gtk-4.0
# GTK4-specific widgets live here; GLib and public headers are shared in src/.
#

spice_gtk4_include = [include_directories('.', '..')]

# GTK4-specific compiler flags
spice_gtk4_cflags = [
  '-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14',
  '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
]

spice_client_gtk4_headers = [
  '../spice-client-gtk.h',
  '../spice-grabsequence.h',
  '../spice-gtk-session.h',
  '../spice-widget.h',
  '../usb-device-widget.h',
]

install_headers(spice_client_gtk4_headers, subdir : 'spice-client-gtk-4.0')

# generate spice-widget-enums.[ch] for GTK4 (from the shared header)
spice_widget_enums_gtk4 = gnome.mkenums_simple('spice-widget-enums',
                                               sources : '../spice-widget.h',
                                               install_header : true,
                                               body_prefix: '#include "get-type-declarations.h"',
                                               install_dir : spice_gtk_includedir / 'spice-client-gtk-4.0')

spice_client_gtk4_introspection_sources = [
  spice_client_gtk4_headers,
  spice_widget_enums_gtk4,
  '../spice-grabsequence.c',
  'spice-gtk-session.c',
  'spice-widget.c',
  'usb-device-widget.c',
]

spice_client_gtk4_sources = [
  spice_marshals,
  spice_client_gtk4_introspection_sources,
  '../desktop-integration.c',
  '../desktop-integration.h',
  '../spice-file-transfer-task.h',
  '../spice-grabsequence.h',
  '../spice-grabsequence-priv.h',
  '../spice-gtk-session-priv.h',
  '../spice-util.c',
  '../spice-util-priv.h',
  'spice-widget-cairo.c',
  'spice-widget-priv.h',
  'vncdisplaykeymap.c',
  'vncdisplaykeymap.h',
]

if spice_gtk_has_egl
  spice_client_gtk4_sources += 'spice-widget-dmabuf.c'
endif

# keymaps for the GTK4 display widget
keymaps_gtk4 = ['xorgevdev',
                'xorgkbd',
                'xorgxquartz',
                'xorgxwin',
                'osx',
                'win32',
                'x11']

foreach keymap : keymaps_gtk4
  varname = 'keymap_@0@2xtkbd'.format(keymap)
  target = 'vncdisplay@0@_gtk4.h'.format(varname)
  cmd = [keymapgen, 'code-map', '--lang', 'glib2', '--varname', varname, keymapcsv, keymap, 'xtkbd']
  spice_client_gtk4_sources += custom_target(target,
                                             output : target,
                                             capture : true,
                                             build_by_default: true,
                                             command : cmd)
endforeach

#
# Wayland protocols for GTK4
#
if spice_gtk4_has_wayland_protocols

  gtk4_generated_protocols = [
    [ 'pointer-constraints', 'v1' ],
    [ 'relative-pointer', 'v1' ],
  ]

  foreach proto: gtk4_generated_protocols
    proto_name = proto[0]
    if proto[1] == 'internal'
      base_file = proto_name
      xml_path = '@0@.xml'.format(proto_name)
    elif proto[1] == 'stable'
      base_file = proto_name
      xml_path = '@0@/stable/@1@/@1@.xml'.format(dir_wp_base, base_file)
    else
      base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
      xml_path = '@0@/unstable/@1@/@2@.xml'.format(dir_wp_base, proto_name, base_file)
    endif

    foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
      if output_type == 'client-header'
        output_file = '@0@-client-protocol-gtk4.h'.format(base_file)
      elif output_type == 'server-header'
        output_file = '@0@-server-protocol-gtk4.h'.format(base_file)
      else
        output_file = '@0@-protocol-gtk4.c'.format(base_file)
        if dep_scanner.version().version_compare('< 1.14.91')
            output_type = 'code'
        endif
      endif

      spice_client_gtk4_sources += custom_target(
        '@0@ @1@ (gtk4)'.format(base_file, output_type),
        command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
        input: xml_path,
        output: output_file,
      )
    endforeach
  endforeach

  spice_client_gtk4_sources += [
    'wayland-extensions.c',
    'wayland-extensions.h',
  ]
endif

#
# libspice-client-gtk-4.0.so
#

# soversion — fresh start for GTK4 library
spice_client_gtk4_so_version = '0.0.0'
summary_info += {'libspice-client-gtk4.so version': spice_client_gtk4_so_version}

spice_client_gtk4_lib = library('spice-client-gtk-4.0', spice_client_gtk4_sources,
                                version : spice_client_gtk4_so_version,
                                install : true,
                                include_directories : [spice_gtk_include, spice_gtk4_include],
                                c_args : spice_gtk4_cflags,
                                link_args : [spice_gtk_version_script],
                                link_depends : spice_client_gtk_syms,
                                dependencies : [spice_client_glib_dep, spice_gtk4_deps, spice_gtk4_wayland_deps],
                                gnu_symbol_visibility: 'hidden')

spice_client_gtk4_dep = declare_dependency(sources : spice_widget_enums_gtk4[1],
                                           link_with : spice_client_gtk4_lib,
                                           dependencies : [spice_client_glib_dep, spice_gtk4_deps, spice_gtk4_wayland_deps])

if spice_gtk_has_introspection
  #
  # SpiceClientGtk-4.0.gir
  #
  spice_client_gtk4_gir = gnome.generate_gir(spice_client_gtk4_lib,
                                             export_packages : 'spice-client-gtk-4.0',
                                             extra_args : ['--accept-unprefixed'],
                                             header : 'spice-widget.h',
                                             includes : ['GObject-2.0', 'Gtk-4.0', spice_client_glib_gir[0]],
                                             include_directories: [spice_gtk_include, spice_gtk4_include],
                                             link_with : spice_client_glib_lib,
                                             identifier_prefix : 'Spice',
                                             symbol_prefix : 'spice',
                                             install : spice_gtk_has_introspection,
                                             namespace : 'SpiceClientGtk',
                                             nsversion : '4.0',
                                             sources : spice_client_gtk4_introspection_sources)
endif
