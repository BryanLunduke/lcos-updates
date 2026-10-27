project(
  'lcos-updates',
  'cpp',
  version: '1.0.0',
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

# Debug info would otherwise record the build directory. The shipped binary
# must not contain a workspace path.
add_project_arguments(
  '-ffile-prefix-map=' + meson.project_source_root() + '/=./',
  language: 'cpp',
)

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

lcos_updates = executable(
  'lcos-updates',
  'src/main.cpp',
  'src/window.cpp',
  include_directories: inc,
  link_with: apt_parse,
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

install_data(
  'data/org.lunduke.LcosUpdates.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)


# The app icon, org.lunduke.LcosUpdates, in the hicolor theme. The desktop
# file, window and About dialog use it. Absent paths are skipped so a clean
# meson setup still succeeds.
fs = import('fs')
foreach size : ['32x32', '48x48', '128x128', '256x256']
  png = 'data/icons/hicolor' / size / 'apps' / 'org.lunduke.LcosUpdates.png'
  if fs.exists(png)
    install_data(
      png,
      install_dir: get_option('datadir') / 'icons' / 'hicolor' / size / 'apps',
    )
  endif
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
  timeout: 180,
)

test_r10 = executable(
  'test-r10',
  'tests/test-r10.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round10-repair-protocol-counts',
  test_r10,
  args: [helper.full_path()],
  timeout: 120,
)

test_r11 = executable(
  'test-r11',
  'tests/test-r11.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round11-unfinished-lock-repair-offline-sizes',
  test_r11,
  args: [helper.full_path()],
  timeout: 120,
)

test_r12 = executable(
  'test-r12',
  'tests/test-r12.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round12-debconf-notice-and-new-sizes',
  test_r12,
  args: [helper.full_path()],
  timeout: 120,
)

test_r13 = executable(
  'test-r13',
  'tests/test-r13.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round13-debconf-passwords-priority-partial',
  test_r13,
  args: [helper.full_path()],
  timeout: 120,
)

test_r14 = executable(
  'test-r14',
  'tests/test-r14.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round14-cache-reuse-stale-dirs-repair-answers',
  test_r14,
  args: [helper.full_path()],
  timeout: 120,
)

test_r15 = executable(
  'test-r15',
  'tests/test-r15.cpp',
  include_directories: inc,
  link_with: apt_parse,
  install: false,
)

test(
  'round15-restart-cleanup-metainfo',
  test_r15,
  args: [helper.full_path(), meson.project_source_root()],
  timeout: 120,
)

test_window = executable(
  'test-window',
  'tests/test-window.cpp',
  'src/window.cpp',
  include_directories: inc,
  link_with: apt_parse,
  dependencies: gtkmm,
  cpp_args: ['-DLCOS_UPDATES_TEST'],
  install: false,
)

xvfb = find_program('xvfb-run', required: true)
test(
  'window',
  xvfb,
  args: ['-a', test_window.full_path(), helper.full_path(), meson.project_source_root() / 'tests' / 'fixtures'],
  depends: helper,
  timeout: 120,
)

test(
  'policy-actions',
  find_program('python3'),
  args: [files('tests/test-policy.py'), files('data/org.lunduke.lcos-updates.policy')],
)

# The production GUI must not contain test-only hooks, systemctl, or a
# workspace path. The release build leaves LCOS_UPDATES_TEST undefined.
test(
  'production-pkexec-has-no-test-override',
  find_program('python3'),
  args: [
    '-c',
    '''import pathlib,sys
bins=[pathlib.Path(p) for p in sys.argv[1:]]
gui_bad=(b"LCOS_UPDATES_TEST_", b"LCOS_UPDATES_TEST_PKEXEC", b"LCOS_UPDATES_TEST_LOGINCTL", b"LCOS_UPDATES_GUI_TIMEOUT_MS", b"LCOS_UPDATES_STALL_NOTICE_SEC", b"LCOS_UPDATES_STALL_INHIBIT_SEC", b"LCOS_UPDATES_FAKE_INHIBIT", b"systemctl", b"/workspace")
helper_bad=(b"LCOS_UPDATES_TEST_", b"systemctl", b"/workspace")
for path, bad in ((bins[0], gui_bad), (bins[1], helper_bad)):
    data=path.read_bytes()
    hit=[item.decode() for item in bad if item in data]
    if hit:
        print(path, hit)
        sys.exit(1)
''',
    lcos_updates.full_path(),
    helper.full_path(),
  ],
)

# Purge deletes /var/lib/lcos-updates. remove and upgrade leave it.
test(
  'postrm-purge',
  find_program('sh'),
  args: [files('tests/test-postrm.sh')],
)
