project(
  'lunduke-edit',
  'cpp',
  version: '0.9.6',
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
x11_dep = dependency('x11')

lunduke_edit_inc = include_directories('src')

lunduke_edit_sources = files(
  'src/main.cpp',
  'src/application.cpp',
  'src/main_window.cpp',
  'src/line_gutter.cpp',
  'src/find_replace_dialog.cpp',
)

# Production binary. No test-hook define: dialogs, limits, and chunk sizes
# stay the real defaults, and LUNDUKE_EDIT_TEST_* is not linked in.
lunduke_edit = executable(
  'lunduke-edit',
  lunduke_edit_sources,
  dependencies: [gtkmm_dep, gtksourceviewmm_dep, x11_dep],
  include_directories: lunduke_edit_inc,
  install: true,
)

# Same editor sources as above, plus the behavior tests. Only this target
# defines LUNDUKE_EDIT_TEST_HOOKS.
edit_behavior_tests = executable(
  'edit-behavior-tests',
  files(
    'tests/edit_tests.cpp',
    'src/application.cpp',
    'src/main_window.cpp',
    'src/line_gutter.cpp',
    'src/find_replace_dialog.cpp',
  ),
  cpp_args: ['-DLUNDUKE_EDIT_TEST_HOOKS'],
  dependencies: [gtkmm_dep, gtksourceviewmm_dep, x11_dep],
  include_directories: lunduke_edit_inc,
)

# Drag source for the XDND test. Not installed.
edit_dnd_source = executable(
  'edit-dnd-source',
  files('tests/dnd_source.cpp'),
  dependencies: [gtkmm_dep, x11_dep],
  install: false,
)

# A sanitizer build runs about ten times slower. Stretch the behavior
# tests' waits with it, take full allocation stacks, and suppress only
# toolkit-owned leaks (tests/lsan.supp). The production-binary scan checks
# a release build; a sanitizer build keeps source paths on purpose.
sanitize = get_option('b_sanitize')
behavior_env = environment()
if sanitize.contains('address')
  behavior_env.set('LUNDUKE_EDIT_TEST_TIME_SCALE', '10')
  behavior_env.set('ASAN_OPTIONS',
    'halt_on_error=1:abort_on_error=1:print_summary=1:fast_unwind_on_malloc=0:malloc_context_size=40')
  behavior_env.set('LSAN_OPTIONS',
    'suppressions=' + meson.current_source_dir() / 'tests' / 'lsan.supp' +
    ':print_suppressions=1:fast_unwind_on_malloc=0')
endif
test('behavior', edit_behavior_tests, timeout: sanitize == 'none' ? 3600 : 36000,
     env: behavior_env)

check_production_binary = find_program('tests/check-production-binary.sh')
if sanitize == 'none'
  test(
    'production-binary',
    check_production_binary,
    args: [lunduke_edit],
    depends: lunduke_edit,
  )
endif

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
