# Changelog

All notable changes to GOG Manager are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses
[semantic versioning](https://semver.org/).

## [Unreleased]

### Added
- Box art and details from GOG's games database (gamesdb.gog.com): grid
  cards show the game's vertical cover (the square icon when there is
  none), and the game page shows the background art, developer, publisher,
  series, themes and game modes, and GOG's summary when the game has no
  description. Fetched in the background with the categories and kept in
  the config folder (`gog-manager-gamesdb.json` and `gamesdb-images/`), so
  they work offline; Preferences fetches them again with the categories.
  Only what is missing is fetched: details once per game, then the images
  not on disk yet. Covers appear on the cards as they arrive, and a restart
  carries on where the last run stopped.

### Changed
- The game page lists its details (installer languages, release date,
  categories, developer, publisher, series, themes, modes) as a
  two-column table instead of a block of grey lines, the top line reads
  "Windows · 4,6 GB · Installed", and the banner keeps the art's own
  proportions instead of cropping it to a fixed height.
- Descriptions no longer keep the stray indents and double spaces in
  GOG's HTML.
- Install buttons say "Windows" and "Linux", not "windows" and "linux".
- Preferences is split into three pages, Library, Runners and Downloads,
  with the switcher in the header (a bar at the bottom in a narrow window).
  Folder rows show the path as their main text and open the folder chooser
  from anywhere on the row; the default runner's name gets a line of its
  own; custom runners are listed one per row and can be removed; shorter
  texts throughout.
- The progress of fetching categories, covers and details from GOG, and
  any failure, is shown in a blue bar along the bottom of the window
  instead of small text in the sidebar. A failure can be dismissed.
- The config, install state, download choices, last GOG check and caches are
  kept in a hidden `.gog-manager-configs` folder next to the AppImage instead
  of directly beside it. Files from an older version are moved there on start.

## [0.4.0] - 2026-10-20

### Added
- A `play.sh` in each installed game's folder starts the game with the same
  task, runner and prefix as Play, without gog-manager. It is rewritten after
  an install, a runner change and on every launch; a `play.sh` gog-manager did
  not write is left alone.
- Categories: each game's genres are fetched from GOG in the background after
  a scan and cached in `gog-manager-categories.json`. The sidebar can filter
  by one or more categories (any or all of them), Sort By has a Category
  option, cards show the main genre, and Preferences can fetch them all again.
- Sort by release date, newest or oldest first; the game page shows the date.
- An installed game's page can add it to the Applications menu. The entry
  runs its `play.sh`; uninstalling removes it.
- The library can be shown as a grid of covers, a simple list or a detailed
  table (categories, platforms, status, size, release date, languages, DLC
  and extras). The choice is remembered.
- "Newest Proton" default runner: the newest Proton on the computer, judged by
  the version each build records, so Heroic's older "GE-Proton-latest" folder
  isn't taken for it. A config without a default gets it.

### Changed
- The main category comes before the platforms on cards and rows.
- Screen-reader announcements go only through AT-SPI, so sessions without an
  AT-SPI bus (Broadway, some headless ones) no longer crash.
- The desktop entry lists only the `Game` category, so the app no longer
  shows up twice in some menus.

### Fixed
- Closing the main window no longer crashes (the game list was freed twice).
- A DLC no longer points at a freed base game when the library is rescanned
  during its install.
- Uninstall also protects the home, user and installer folders when they are
  reached through a symlink (e.g. `/home` → `/var/home` on Fedora Atomic).
- `play.sh` quotes each launch argument and keeps the game title on its
  comment line, so neither can run as shell commands.
- Applications menu entries work for game folders with `%` in the name.
- A rescan during the first category fetch no longer freezes the window.
- Callbacks that finish after a window closed (scans, category lookups,
  folder pickers, launches, the Preferences and GE-Proton windows) no longer
  touch freed memory.
- A game description with invalid UTF-8 no longer crashes the app.
- Cancel stops the original-language lookup at once instead of after up to
  45 seconds.
- The GOG cleanup no longer treats files in a neighbouring folder
  (`game_goty` next to `game`) as part of the game.
- Cancelling a GE-Proton download says "Cancelled" instead of "curl was
  terminated (signal 9)"; closing the window stops the download, and the
  progress bar and log have accessible names.
- Preferences no longer saves a default runner that was just removed in
  Manage GE-Proton.
- After an install or uninstall, the game page refreshes even when saving its
  state fails, and keyboard focus moves to the new buttons.
- A product slug that would lead outside the install folder is ignored.

### Release
- The strip check also refuses symbol tables under other names,
  `.gnu_debugdata` (MiniDebugInfo) and extended section numbering.
- The AppImage build stops if its patch to the GTK plugin no longer applies.

## [0.3.0] - 2026-10-18

### Added
- New Games and Updates on GOG, through
  [lgogdownloader](https://github.com/Sude-/lgogdownloader): checks GOG for
  games not downloaded yet and for new versions, lists each file under Game,
  DLC and Extras with its size and language, and downloads the chosen ones
  with overall and per-file progress. Failed files are reported with a
  readable reason and can be tried again later or ignored. Cards show
  "Update on GOG", and the Status filter has "New version on GOG".
- Download languages: by default each game's original language (looked up on
  Wikidata), plus any languages ticked in Preferences; each game can have its
  own choice.
- DLC in lgogdownloader's `<game>/dlc/<dlc>/` folders is found and listed on
  its game's page.
- DLC and Extras filters in the sidebar; cards say how many of each a game
  has.
- Manage GE-Proton: lists downloaded builds with their sizes and removes the
  chosen ones, moving games that used them to the newest remaining one.
- GE-Proton downloads are checked against the published SHA-512 checksum.
  Preferences says when a newer GE-Proton is out, and after a download offers
  to remove older versions no game uses.
- An installed Windows game's page can switch its Wine or Proton version
  without reinstalling.
- Each installer's language is read from GOG's file name; a Language sort
  groups games by it.
- Windows installs show the installer's progress and warn when it has
  stalled for two minutes.
- When Wine has no FreeType, the install pauses and shows the command that
  installs it on this distribution.
- Non-Latin language names are shown with their English name, e.g.
  "Українська (Ukrainian)".

### Changed
- Every dialog is a window of its own that can be moved and resized.
- The app's icons are built in, so none go missing under other icon themes.
- Uninstall never deletes the standard user folders, a mount point or the
  folder with the game's installers; a non-empty install folder gets a
  subfolder named after the game.

### Fixed
- GE-Proton downloads work with the new per-architecture archives; the newest
  real release is picked, and a half-unpacked build can't pass for installed.
- Proton installs set up their prefix (the prefix folder is created first);
  launching repairs a missing prefix.
- Cancelling an install stops the installer and Wine's own processes.
- "Show installed files" and the extras' open buttons open the folder or file.
- An install that finishes after a rescan shows as installed.
- Each language appears once in the language filter.
- Download Selected could fetch the wrong game from the updates list.
- Wine/Proton launch arguments are split by Windows rules, so backslashes
  are kept.

## [0.2.0] - 2026-10-16

### Added
- Serial keys from a game's `serials.txt` are shown on its page, with a copy
  button.
- Custom Wine and Proton runners can be added in Preferences.
- Accessible names for icon-only buttons and dropdowns.

### Fixed
- Launching installed Windows games: the runner, its path and the prefix are
  now saved at install time.
- Games install into the folder shown in the install dialog, and `c:\game`
  points at the right folder.
- GOG `.sh` installers no longer fail on unzip's warning exit status;
  installers are unpacked next to the destination instead of in `/tmp`.
- Uninstalling a DLC no longer deletes its base game; uninstall refuses `/`
  and the home folder.
- Memory corruption after install or uninstall, game objects freed by a
  rescan, overlapping scans, and an endless spinner on scan errors.
- Changing the library path in Preferences is saved.
- Release binaries are stripped, and a release with any unstripped ELF is
  refused.

## [0.1.0] - 2026-08-07

### Added
- First release: a GTK 4 / libadwaita app to browse, install and launch a
  folder of GOG offline installers, with Wine and Proton support and a
  GE-Proton downloader.
- Config and install state live next to the executable, so a portable copy
  keeps them.
- Single-file AppImage, built and published by CI.

[Unreleased]: https://github.com/frebergguru/gog-manager/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/frebergguru/gog-manager/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/frebergguru/gog-manager/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/frebergguru/gog-manager/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/frebergguru/gog-manager/releases/tag/v0.1.0
