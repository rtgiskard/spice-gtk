tests_sources = [
  'util.c',
  'coroutine.c',
  'session.c',
  'uri.c',
  'file-transfer.c',
]

if spice_gtk_has_phodav
  tests_sources += 'pipe.c'
endif

if spice_gtk_has_usbredir
  tests_sources += 'cd-emu.c'
endif

if spice_gtk_has_polkit
  tests_sources += [
    'usb-acl-helper.c',
    'mock-acl-helper.c',
  ]
endif

# create a static library from a shared one extracting all objects
# this allows to rewrite part of it if necessary for mocking
test_lib = static_library('test-lib',
                          objects : spice_client_glib_lib.extract_all_objects(recursive: true))

foreach src : tests_sources
  name = 'test-@0@'.format(src).split('.')[0]
  exe = executable(name,
                   sources : src,
                   c_args : '-DTESTDIR="@0@"'.format(meson.current_build_dir()),
                   link_with : test_lib,
                   dependencies : spice_client_glib_dep)
  if not name.contains('mock-acl-helper')
    test(name, exe)
  endif
endforeach

# The GTK4 display's physical-size conversion is pure and needs no display or VM.
widget_geometry_test = executable('test-widget-geometry', 'widget-geometry.c',
                                  include_directories : spice_gtk4_include,
                                  c_args : spice_gtk4_cflags + ['-DSPICE_COMPILATION'],
                                  dependencies : spice_client_gtk4_dep)
test('test-widget-geometry', widget_geometry_test)

widget_rendering_test = executable('test-widget-rendering', 'widget-rendering.c',
                                   include_directories : spice_gtk4_include,
                                   c_args : spice_gtk4_cflags + ['-DSPICE_COMPILATION'],
                                   dependencies : spice_client_gtk4_dep)
test('test-widget-rendering', widget_rendering_test,
     env : ['GSETTINGS_BACKEND=memory', 'GTK_A11Y=none'])

gtk4_x11_test_dep = dependency('gtk4-x11', required: false)
xtst_test_dep = dependency('xtst', required: false)
x11_test_dep = dependency('x11', required: false)
if gtk4_x11_test_dep.found() and xtst_test_dep.found() and x11_test_dep.found()
  widget_input_test = executable('test-widget-input', 'widget-input.c',
                                 include_directories : spice_gtk4_include,
                                 c_args : spice_gtk4_cflags + ['-DSPICE_COMPILATION'],
                                 dependencies : [spice_client_gtk4_dep, gtk4_x11_test_dep,
                                                 xtst_test_dep, x11_test_dep])
  test('test-widget-input', widget_input_test,
       env : ['GSETTINGS_BACKEND=memory', 'GTK_A11Y=none'])
endif

clipboard_test = executable('test-gtk-clipboard',
                            ['gtk-clipboard.c', '../src/desktop-integration.c'],
                            include_directories : [spice_gtk_include, spice_gtk4_include],
                            c_args : spice_gtk4_cflags + ['-DSPICE_COMPILATION'],
                            link_with : test_lib,
                            dependencies : [spice_client_glib_dep, spice_gtk4_deps])
test('test-gtk-clipboard', clipboard_test,
     env : ['GSETTINGS_BACKEND=memory', 'GTK_A11Y=none'])
