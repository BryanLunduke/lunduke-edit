project(
  'lunduke-edit',
  'cpp',
  version: '0.3',
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
