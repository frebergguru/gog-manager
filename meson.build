# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Hypnotize

project('gog-manager', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  license_files : 'LICENSE',
  default_options : ['c_std=gnu17', 'warning_level=2'])

gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')
gio_dep = dependency('gio-2.0')
json_dep = dependency('json-c')

gnome = import('gnome')

resources = gnome.compile_resources(
  'gog-resources',
  'src/resources/gog-manager.gresource.xml',
  source_dir : 'src/resources',
  c_name : 'gog_resources'
)

common_sources = [
  'src/json_util.c',
  'src/text_util.c',
  'src/game.c',
  'src/library.c',
  'src/config.c',
  'src/runner.c',
  'src/proton_fetch.c',
  'src/installer_engine.c',
  'src/launch.c',
]

ui_sources = [
  'src/ui/main_window.c',
  'src/ui/game_detail.c',
  'src/ui/install_dialog.c',
  'src/ui/proton_fetch_dialog.c',
  'src/ui/preferences_dialog.c',
]

executable('gog-manager',
  ['src/main.c'] + common_sources + ui_sources + resources,
  dependencies : [gtk_dep, adw_dep, gio_dep, json_dep],
  install : true,
)

# Desktop entry and icon — required for AppImage packaging and for a normal
# system install to show up in application menus. See packaging/build-appimage.sh.
install_data('packaging/org.gogmanager.app.desktop',
  install_dir : get_option('datadir') / 'applications')
install_data('packaging/org.gogmanager.app.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

test_deps = [gtk_dep, adw_dep, gio_dep, json_dep]
test_support_sources = ['src/json_util.c', 'src/text_util.c', 'src/runner.c', 'src/proton_fetch.c',
                         'src/config.c', 'src/game.c']

test_json_exe = executable('test-json-util',
  ['tests/test_json_util.c'] + test_support_sources,
  dependencies : test_deps,
)
test('json-util', test_json_exe)

test_runner_exe = executable('test-runner-cmdline',
  ['tests/test_runner_cmdline.c'] + test_support_sources,
  dependencies : test_deps,
)
test('runner-cmdline', test_runner_exe)
