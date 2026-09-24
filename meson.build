project(
  'lunduke-edit',
  'cpp',
  version: '0.7.3',
  license: 'GPL-3.0-or-later',
  meson_version: '>=0.56.0',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
    'werror=false',
  ],
)

gtkmm_dep = dependency('gtkmm-3.0', version: '>=3.24')
gtksourceviewmm_dep = dependency('gtksourceviewmm-3.0', version: '>=3.18')

lunduke_edit_inc = include_directories('src')

lunduke_edit_sources = files(
  'src/main.cpp',
  'src/application.cpp',
  'src/main_window.cpp',
  'src/line_gutter.cpp',
  'src/find_replace_dialog.cpp',
)

executable(
  'lunduke-edit',
  lunduke_edit_sources,
  dependencies: [gtkmm_dep, gtksourceviewmm_dep],
  include_directories: lunduke_edit_inc,
  install: true,
)

install_data(
  'data/org.lunduke.LundukeEdit.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'),
)

install_data(
  'data/org.lunduke.LundukeEdit.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo'),
)

# App icon: hicolor PNGs are authoritative (Paint pattern). Install only sizes
# that exist — Bob owns the artwork; see data/icons/README.md.
# Do not install data/icons/org.lunduke.LundukeEdit-master.png (1024 source).
fs = import('fs')
app_icon_sizes = ['16', '22', '24', '32', '48', '64', '96', '128', '256', '512']
foreach size : app_icon_sizes
  icon = 'data/icons/hicolor/' + size + 'x' + size + '/apps/org.lunduke.LundukeEdit.png'
  if fs.exists(icon)
    install_data(
      icon,
      install_dir: join_paths(get_option('datadir'), 'icons/hicolor/' + size + 'x' + size + '/apps'),
    )
  endif
endforeach
