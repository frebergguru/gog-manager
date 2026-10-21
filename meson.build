# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Hypnotize

project('gog-manager', 'c',
  version : '0.7.0',
  license : 'GPL-3.0-or-later',
  license_files : 'LICENSE',
  default_options : ['c_std=gnu17', 'warning_level=2'])

# GtkFileDialog/GtkUriLauncher need GTK 4.10; AdwSpinner needs libadwaita 1.6.
gtk_dep = dependency('gtk4', version : '>= 4.10')
adw_dep = dependency('libadwaita-1', version : '>= 1.6')
gio_dep = dependency('gio-2.0')
json_dep = dependency('json-c')
# GOG Galaxy's cloud saves (src/cloud_saves.c).
curl_dep = dependency('libcurl')
m_dep = meson.get_compiler('c').find_library('m', required : false)
# Optional: GOG's login page inside GOG Manager (see src/ui/login_dialog.c).
# Without it, the login opens in the web browser and the address is pasted.
webkit_dep = dependency('webkitgtk-6.0', required : get_option('webkit'))
if webkit_dep.found()
  add_project_arguments('-DHAVE_WEBKIT', language : 'c')
endif

add_project_arguments('-DGOG_MANAGER_VERSION="@0@"'.format(meson.project_version()),
  language : 'c')

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
  'src/gog_sync.c',
  'src/original_language.c',
  'src/categories.c',
  'src/gamesdb.c',
  'src/fetch.c',
  'src/installer_engine.c',
  'src/launch.c',
  'src/shortcut.c',
  'src/recommend.c',
  'src/cloud_saves.c',
]

ui_sources = [
  'src/ui/main_window.c',
  'src/ui/game_detail.c',
  'src/ui/install_dialog.c',
  'src/ui/proton_fetch_dialog.c',
  'src/ui/proton_manage_dialog.c',
  'src/ui/gog_sync_dialog.c',
  'src/ui/preferences_dialog.c',
  'src/ui/dialog_window.c',
  'src/ui/repair_dialog.c',
  'src/ui/cloud_saves_dialog.c',
  'src/ui/for_you.c',
  'src/ui/login_dialog.c',
]

executable('gog-manager',
  ['src/main.c'] + common_sources + ui_sources + resources,
  dependencies : [gtk_dep, adw_dep, gio_dep, json_dep, webkit_dep, m_dep, curl_dep],
  install : true,
)

# Desktop entry and icon — required for AppImage packaging and for a normal
# system install to show up in application menus. See packaging/build-appimage.sh.
install_data('packaging/org.gogmanager.app.desktop',
  install_dir : get_option('datadir') / 'applications')
# GOG Manager's license, and the LGPL the Adwaita icons in its resources
# are used under.
install_data('LICENSE', 'LICENSES/LGPL-3.0-only.txt',
  install_dir : get_option('datadir') / 'licenses' / 'gog-manager')
install_data('packaging/org.gogmanager.app.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

test_deps = [gtk_dep, adw_dep, gio_dep, json_dep, m_dep, curl_dep]
test_support_sources = ['src/json_util.c', 'src/text_util.c', 'src/runner.c', 'src/proton_fetch.c',
                         'src/gog_sync.c', 'src/original_language.c', 'src/categories.c',
                         'src/gamesdb.c', 'src/fetch.c', 'src/config.c', 'src/game.c',
                         'src/recommend.c', 'src/cloud_saves.c']

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

test_library_exe = executable('test-library',
  ['tests/test_library.c', 'src/library.c', 'src/installer_engine.c'] + test_support_sources,
  dependencies : test_deps,
)
test('library', test_library_exe)

test_play_script_exe = executable('test-play-script',
  ['tests/test_play_script.c', 'src/launch.c'] + test_support_sources,
  dependencies : test_deps,
)
test('play-script', test_play_script_exe)

test_categories_exe = executable('test-categories',
  ['tests/test_categories.c'] + test_support_sources,
  dependencies : test_deps,
)
test('categories', test_categories_exe)

test_gamesdb_exe = executable('test-gamesdb',
  ['tests/test_gamesdb.c'] + test_support_sources,
  dependencies : test_deps,
)
test('gamesdb', test_gamesdb_exe)

test_shortcut_exe = executable('test-shortcut',
  ['tests/test_shortcut.c', 'src/shortcut.c', 'src/launch.c'] + test_support_sources,
  dependencies : test_deps,
)
test('shortcut', test_shortcut_exe)

test_recommend_exe = executable('test-recommend',
  ['tests/test_recommend.c'] + test_support_sources,
  dependencies : test_deps,
)
test('recommend', test_recommend_exe)

test_cloud_saves_exe = executable('test-cloud-saves',
  ['tests/test_cloud_saves.c'] + test_support_sources,
  dependencies : test_deps,
)
test('cloud-saves', test_cloud_saves_exe)
