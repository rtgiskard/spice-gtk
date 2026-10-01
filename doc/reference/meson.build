toml_conf = configuration_data()
# Extract a clean major.minor.micro version for gi-docgen,
# as it cannot parse git-describe suffixes like "0.42.51-942a"
_doc_version_info = meson.project_version().split('.')
_doc_micro = '0'
if _doc_version_info.length() > 2
  _doc_micro = _doc_version_info[2].split('-')[0]
endif
toml_conf.set('VERSION', '.'.join([_doc_version_info[0], _doc_version_info[1], _doc_micro]))

gidocgen = find_program('gi-docgen')

docs_dir = spice_gtk_datadir / 'doc'

glib_content_files = files(
  'overview.md',
  'session-channels.md',
  'application-support.md',
)

glib_toml = configure_file(
  input: 'SpiceClientGLib.toml.in',
  output: 'SpiceClientGLib.toml',
  configuration: toml_conf,
)

custom_target('spice-client-glib-doc',
  input: [glib_toml, spice_client_glib_gir[0]],
  output: 'SpiceClientGLib-2.0',
  command: [
    gidocgen,
    'generate',
    '--quiet',
    '--no-namespace-dir',
    '--add-include-path=@0@'.format(meson.project_build_root() / 'src'),
    '--config=@INPUT0@',
    '--output-dir=@OUTPUT@',
    '--content-dir=@0@'.format(meson.current_source_dir()),
    '@INPUT1@',
  ],
  depends: spice_client_glib_gir[0],
  depend_files: glib_content_files,
  build_by_default: true,
  install: true,
  install_dir: docs_dir,
)

gtk_content_files = files('gtk-overview.md')

gtk_toml = configure_file(
  input: 'SpiceClientGtk.toml.in',
  output: 'SpiceClientGtk.toml',
  configuration: toml_conf,
)

custom_target('spice-client-gtk-doc',
  input: [gtk_toml, spice_client_gtk4_gir[0]],
  output: 'SpiceClientGtk-4.0',
  command: [
    gidocgen,
    'generate',
    '--quiet',
    '--no-namespace-dir',
    '--add-include-path=@0@'.format(meson.project_build_root() / 'src'),
    '--config=@INPUT0@',
    '--output-dir=@OUTPUT@',
    '--content-dir=@0@'.format(meson.current_source_dir()),
    '@INPUT1@',
  ],
  depends: [spice_client_gtk4_gir[0], spice_client_glib_gir[0]],
  depend_files: gtk_content_files,
  build_by_default: true,
  install: true,
  install_dir: docs_dir,
)
