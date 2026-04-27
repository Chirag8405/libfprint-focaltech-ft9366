project(
  'libfprint-focaltech-ft9366',
  'c',
  version: '0.1.0',
  license: 'LGPL-2.1-or-later',
  default_options: ['c_std=c17', 'warning_level=2']
)

glib_dep = dependency('glib-2.0', required: true)
gobject_dep = dependency('gobject-2.0', required: true)
gusb_dep = dependency('gusb', required: true)
nss_dep = dependency('nss', required: true)
libfprint_dep = dependency('libfprint-2')

src_inc = include_directories('src')

shared_library(
  'fprint-focaltech-ft9366',
  [
    'src/focaltech-ft9366.c',
    'src/focaltech-crypto.c',
    'src/focaltech-usb.c',
  ],
  include_directories: src_inc,
  dependencies: [libfprint_dep, glib_dep, gobject_dep, gusb_dep, nss_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'libfprint-2', 'tod-1'),
  version: meson.project_version(),
  soversion: '0'
)

if get_option('install_udev_rules')
  install_data(
    'udev/60-libfprint-2-focaltech.rules',
    install_dir: get_option('udev_rules_dir')
  )
endif
