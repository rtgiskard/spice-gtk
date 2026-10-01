spice_gtk_include += [include_directories('.')]

#
# Source files for spice-client-glib
#

# generate spice-version.h
if meson.project_version().startswith('UNKNOWN')
  major = '0'
  minor = '0'
  micro = '1'
else
  version_info = meson.project_version().split('.')
  major = '@0@'.format(version_info[0])
  minor = '@0@'.format(version_info[1].split('-')[0])
  micro = '0'
  if version_info.length() > 2
    micro = version_info[2].split('-')[0]
  endif
endif

version_data = configuration_data()
version_data.set('SPICE_GTK_MAJOR_VERSION', major)
version_data.set('SPICE_GTK_MINOR_VERSION', minor)
version_data.set('SPICE_GTK_MICRO_VERSION', micro)

script = ''
# versions, [major, minimum_minor, maximum_minor]
versions = [[ 0, 1, 43 ]]
# build a long string to use for loops
x = 'aaaaaaaaaa'
x = x.replace('a', 'bbbbbbbbbb')

# loop all versions, generate SPICE_GTK_VERSION_major_minor macros
foreach version : versions
  major = version[0]
  minor = version[1]
  max_minor = version[2]
  foreach c : x.split('b')
    script = '''@0@
#define SPICE_GTK_VERSION_@1@_@2@ SPICE_GTK_ENCODE_VERSION(@1@, @2@)
'''.format(script, major, minor)
    minor += 1
    if minor > max_minor
      break
    endif
  endforeach
endforeach

versions = '''2 3 4 5 6 7 8 9 10 11 13 14 15 17 19
   20 24 27 30 31 32 33 34 35 36 38 39 40 43'''.split()
foreach version: versions
  script = '''@2@
#if SPICE_GTK_VERSION_MAX_ALLOWED < SPICE_GTK_VERSION_@0@_@1@
# define SPICE_GTK_AVAILABLE_IN_@0@_@1@  G_UNAVAILABLE(@0@, @1@)
#else
# define SPICE_GTK_AVAILABLE_IN_@0@_@1@  SPICE_GTK_EXTERN
#endif

#if SPICE_GTK_VERSION_MIN_REQUIRED >= SPICE_GTK_VERSION_@0@_@1@
# define SPICE_GTK_DEPRECATED_IN_@0@_@1@         G_DEPRECATED
# define SPICE_GTK_DEPRECATED_IN_@0@_@1@_FOR(f)  G_DEPRECATED_FOR(f)
#else
# define SPICE_GTK_DEPRECATED_IN_@0@_@1@         SPICE_GTK_EXTERN
# define SPICE_GTK_DEPRECATED_IN_@0@_@1@_FOR(f)  SPICE_GTK_EXTERN
#endif
'''.format('0', version, script)
endforeach

version_data.set('SPICE_VERSION_MACROS', script)
spice_version_h = configure_file(input : 'spice-version.h.in',
                                 output : 'spice-version.h',
                                 configuration : version_data)

spice_client_glib_headers = [
  spice_version_h,
  'channel-cursor.h',
  'channel-display.h',
  'channel-inputs.h',
  'channel-main.h',
  'channel-playback.h',
  'channel-port.h',
  'channel-record.h',
  'channel-smartcard.h',
  'channel-usbredir.h',
  'channel-webdav.h',
  'qmp-port.h',
  'smartcard-manager.h',
  'spice-audio.h',
  'spice-channel.h',
  'spice-client.h',
  'spice-file-transfer-task.h',
  'spice-option.h',
  'spice-session.h',
  'spice-types.h',
  'spice-uri.h',
  'spice-util.h',
  'usb-device-manager.h',
]

install_headers(spice_client_glib_headers, subdir : 'spice-client-glib-2.0')

# generate spice-marshal.[ch]
gnome = import('gnome')
spice_marshals = gnome.genmarshal('spice-marshal', sources : 'spice-marshal.txt')

# generate spice-glib-enums.[ch]
spice_client_glib_enums = gnome.mkenums_simple('spice-glib-enums',
                                               sources : ['spice-channel.h', 'channel-inputs.h', 'spice-session.h'],
                                               install_header : true,
                                               body_prefix: '#include "get-type-declarations.h"',
                                               install_dir : spice_gtk_includedir / 'spice-client-glib-2.0')

spice_client_glib_introspection_sources = [
  spice_client_glib_headers,
  spice_client_glib_enums,
  'channel-cursor.c',
  'channel-display.c',
  'channel-inputs.c',
  'channel-main.c',
  'channel-playback.c',
  'channel-port.c',
  'channel-record.c',
  'channel-smartcard.c',
  'channel-usbredir.c',
  'channel-webdav.c',
  'qmp-port.c',
  'smartcard-manager.c',
  'spice-audio.c',
  'spice-channel.c',
  'spice-client.c',
  'spice-option.c',
  'spice-session.c',
  'spice-util.c',
  'spice-util-glib.c',
  'usb-device-manager.c',
]

spice_client_glib_sources = [
  spice_marshals,
  spice_client_glib_introspection_sources,
  'bio-gio.c',
  'bio-gio.h',
  'channel-base.c',
  'channel-display-gst.c',
  'channel-display-priv.h',
  'channel-playback-priv.h',
  'channel-usbredir-priv.h',
  'client_sw_canvas.c',
  'client_sw_canvas.h',
  'coroutine.h',
  'decode-glz.c',
  'decode.h',
  'decode-jpeg.c',
  'decode-zlib.c',
  'gio-coroutine.c',
  'gio-coroutine.h',
  'qmp-port.c',
  'qmp-port.h',
  'smartcard-manager-priv.h',
  'spice-audio-priv.h',
  'spice-channel-cache.h',
  'spice-channel-priv.h',
  'spice-common.h',
  'spice-file-transfer-task.c',
  'spice-file-transfer-task-priv.h',
  'spice-glib-main.c',
  'spice-gstaudio.c',
  'spice-gstaudio.h',
  'spice-option.h',
  'spice-session-priv.h',
  'spice-uri.c',
  'spice-uri-priv.h',
  'spice-util-priv.h',
  'usb-device-manager-priv.h',
  'vmcstream.c',
  'vmcstream.h',
]

if spice_gtk_has_builtin_mjpeg
  spice_client_glib_sources += 'channel-display-mjpeg.c'
endif

if spice_gtk_has_polkit
  spice_client_glib_sources += ['usb-acl-helper.c',
                                'usb-acl-helper.h']
endif

if spice_gtk_has_phodav
  spice_client_glib_sources += ['giopipe.c',
                                'giopipe.h']
endif

if spice_gtk_coroutine == 'gthread'
  spice_client_glib_sources += 'coroutine_gthread.c'
elif spice_gtk_coroutine in ['ucontext', 'libucontext']
  spice_client_glib_sources += ['continuation.c',
                                'continuation.h',
                                'coroutine_ucontext.c']
elif spice_gtk_coroutine == 'winfiber'
  spice_client_glib_sources += 'coroutine_winfibers.c'
endif

if spice_gtk_has_usbredir
  spice_client_glib_sources += [
    'usbutil.c',
    'usbutil.h',
    'usb-backend.c',
    'usb-emulation.h',
    'usb-backend.h',
    'usb-device-cd.c',
    'usb-device-cd.h',
    'cd-scsi.c',
    'cd-scsi.h',
    'cd-scsi-dev-params.h',
    'cd-usb-bulk-msd.c',
    'cd-usb-bulk-msd.h',
  ]
endif

if spice_gtk_has_usbredir and host_machine.system() == 'windows'
  spice_client_glib_sources += ['usbdk_api.c',
                                'usbdk_api.h']
endif

#
# libspice-client-glib-2.0.so
#

# version-script
test_syms_path = meson.current_source_dir() / 'test-map-file'
test_version_script = '-Wl,--version-script=@0@'.format(test_syms_path)
spice_has_version_script = compiler.has_link_argument(test_version_script)

if spice_has_version_script
  spice_client_glib_syms = files('map-file-glib')
  spice_client_gtk_syms = files('map-file-gtk')
  spice_client_glib_syms_path = meson.current_source_dir() / 'map-file-glib'
  spice_client_gtk_syms_path = meson.current_source_dir() / 'map-file-gtk'
  spice_glib_version_script = '-Wl,--version-script=@0@'.format(spice_client_glib_syms_path)
  spice_gtk_version_script = '-Wl,--version-script=@0@'.format(spice_client_gtk_syms_path)
else
  if host_machine.system() == 'linux'
    error('Version scripts should be supported on Linux')
  endif
  spice_client_glib_syms = []
  spice_client_gtk_syms = []
  spice_glib_version_script = []
  spice_gtk_version_script = []
endif

# soversion
# http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
spice_client_glib_current = 17
spice_client_glib_revision = 0
spice_client_glib_age = 9
spice_client_glib_so_version = '@0@.@1@.@2@'.format(spice_client_glib_current - spice_client_glib_age,
                                                    spice_client_glib_age,
                                                    spice_client_glib_revision)
summary_info += {'libspice-client-glib.so version': spice_client_glib_so_version}

spice_client_glib_lib = library('spice-client-glib-2.0', spice_client_glib_sources,
                                version : spice_client_glib_so_version,
                                install : true,
                                include_directories : spice_gtk_include,
                                link_args : [spice_glib_version_script],
                                link_depends : spice_client_glib_syms,
                                dependencies : spice_glib_deps,
                                gnu_symbol_visibility: 'hidden')

spice_client_glib_dep = declare_dependency(sources : [spice_marshals[1], spice_client_glib_enums[1]],
                                           link_with : spice_client_glib_lib,
                                           include_directories : spice_gtk_include,
                                           dependencies : spice_glib_deps)

#
# SpiceClientGLib-2.0.gir
#
if spice_gtk_has_introspection
spice_client_glib_gir = gnome.generate_gir(spice_client_glib_lib,
                                           export_packages : 'spice-client-glib-2.0',
                                           extra_args : ['--accept-unprefixed'],
                                           header : 'spice-client.h',
                                           includes : ['GObject-2.0', 'Gio-2.0', 'GstBase-1.0'],
                                           identifier_prefix : 'Spice',
                                           include_directories: spice_gtk_include,
                                           link_with : spice_client_glib_lib,
                                           symbol_prefix : 'spice',
                                           install : spice_gtk_has_introspection,
                                           namespace : 'SpiceClientGLib',
                                           nsversion : '2.0',
                                           sources : spice_client_glib_introspection_sources)
endif
#
# spice-client-glib-usb-acl-helper
#
if spice_gtk_has_polkit
  executable('spice-client-glib-usb-acl-helper',
             'spice-client-glib-usb-acl-helper.c',
             include_directories : spice_gtk_include,
             install : true,
             install_dir : spice_gtk_usb_acl_helper_dir,
             dependencies : spice_acl_deps,
             pie : spice_gtk_has_pie
            )
  meson.add_install_script('../build-aux/setcap-or-suid', spice_gtk_usb_acl_helper_dir / 'spice-client-glib-usb-acl-helper', 'cap_fowner+ep')
endif
