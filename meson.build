project('gtkbrowser', 'c',
  version: '1.0.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
  ])

dependencies = [
  dependency('gtk+-3.0'),
  dependency('webkit2gtk-4.1'),
]

executable('gtkbrowser',
  'browser.c',
  'adblock.c',
  'keepass.c',
  dependencies: dependencies,
  c_args: ['-Wno-unused-parameter'],
  link_args: ['-lutil'],
  install: true,
  install_dir: get_option('bindir'))

# Generate the .desktop file with the correct install path
desktop = configure_file(
  input: 'gtkbrowser.desktop.in',
  output: 'gtkbrowser.desktop',
  configuration: {
    'bindir': join_paths(get_option('prefix'), get_option('bindir')),
  })

install_data(desktop,
  install_dir: get_option('datadir') / 'applications')