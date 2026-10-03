project(
  'lcos-updates',
  'cpp',
  version: '0.9',
  license: 'GPL-3.0-or-later',
  meson_version: '>=0.56',
  default_options: [
    'cpp_std=c++17',
    'warning_level=2',
    'prefix=/usr',
  ],
)

gtkmm = dependency('gtkmm-3.0')
inc = include_directories('src')

apt_parse = static_library(
  'apt-parse',
  'src/apt-parse.cpp',
  include_directories: inc,
  install: false,
)

proc_group = static_library(
  'proc-group',
  'src/proc-group.cpp',
  include_directories: inc,
  install: false,
)

executable(
  'lcos-updates',
  'src/main.cpp',
  'src/window.cpp',
  include_directories: inc,
  link_with: [apt_parse, proc_group],
  dependencies: gtkmm,
  install: true,
  install_dir: get_option('bindir'),
)

helper = executable(
  'lcos-updates-helper',
  'src/helper.cpp',
  include_directories: inc,
  link_with: [apt_parse, proc_group],
  install: true,
  install_dir: get_option('libexecdir'),
)

install_data(
  'data/org.lunduke.LcosUpdates.desktop',
  install_dir: get_option('datadir') / 'applications',
)


foreach size : ['32x32', '48x48', '128x128', '256x256']
  install_data(
    'data/icons/hicolor' / size / 'apps' / 'org.lunduke.LcosUpdates.png',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / size / 'apps',
  )
endforeach

install_data(
  'data/org.lunduke.lcos-updates.policy',
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)

test_parser = executable(
  'test-parser',
  'tests/test-parser.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'simulate-parser',
  test_parser,
  args: [meson.project_source_root() / 'tests' / 'fixtures'],
)

test(
  'helper-refuse-dist-upgrade',
  helper,
  args: ['dist-upgrade'],
  should_fail: true,
)

test(
  'helper-refuse-full-upgrade',
  helper,
  args: ['full-upgrade'],
  should_fail: true,
)

test_proc = executable(
  'test-proc-group',
  'tests/test-proc-group.cpp',
  include_directories: inc,
  link_with: proc_group,
  install: false,
)

test(
  'proc-group',
  test_proc,
)

test_helper_kill = executable(
  'test-helper-kill',
  'tests/test-helper-kill.cpp',
  include_directories: inc,
  link_with: proc_group,
  install: false,
)

test(
  'helper-kills-apt-group',
  test_helper_kill,
  args: [helper.full_path()],
  timeout: 40,
)

test(
  'policy-actions',
  find_program('python3'),
  args: [files('tests/test-policy.py'), files('data/org.lunduke.lcos-updates.policy')],
)
