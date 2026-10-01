spice_cmdline_sources = [
  'spice-cmdline.c',
  'spice-cmdline.h',
]

#
# spicy-stats and spicy-screenshot
#
foreach exe : ['spicy-stats', 'spicy-screenshot']
  executable(exe,
             sources : spice_cmdline_sources + ['@0@.c'.format(exe)],
             c_args : '-Wno-deprecated-declarations',
             install : true,
             dependencies : spice_client_glib_dep)
endforeach

# GTK4 smoke client, built but not installed as an end-user viewer.
executable('spicy-test-gtk4',
           sources : ['spicy-test-gtk4.c'] + spice_cmdline_sources,
           c_args : spice_gtk4_cflags,
           install : false,
           dependencies : spice_client_gtk4_dep)
