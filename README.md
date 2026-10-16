# GOG Manager

[![AppImage](https://github.com/frebergguru/gog-manager/actions/workflows/appimage.yml/badge.svg)](https://github.com/frebergguru/gog-manager/actions/workflows/appimage.yml)

A native GTK4 / libadwaita application for Linux that turns a folder of GOG
**offline installers** into a browsable library you can install, launch, and
manage — including Windows-only titles, which it runs transparently through
Wine or Proton.

It never talks to your GOG account or downloads games itself. You point it at
a directory of installers you already own (the `.exe`/`.bin` files and their
`extras/`), and it does the rest locally.

---

## Features

- **Library browser** — scans a directory of GOG products (one subfolder per
  game), reads each product's `product_*.json` metadata, and presents titles,
  logos, descriptions, and sizes in an Adwaita grid.
- **Search, filter, and sort** — live title search; filter by OS
  (Windows / Linux / macOS), by language, and by install status; sort by
  title (A–Z / Z–A), installed-first, or size.
- **One-click install** — installs Linux-native games directly, and drives
  Windows installers unattended through Wine or Proton, laying down a portable
  `c:\game` prefix that keeps game files outside the Wine prefix.
- **Wine / Proton runners** — auto-discovers system Wine and any Proton builds
  under Steam's `compatibilitytools.d` and Heroic's tool directories, plus
  custom runner paths you add in Preferences.
- **Built-in GE-Proton fetcher** — downloads and extracts the latest
  [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom) release into
  a local tools directory, with live progress.
- **DLC & extras** — links DLC products to their base game and installs them
  into the base game's directory; surfaces bonus `extras/` content
  (manuals, soundtracks, wallpapers) per game.
- **Launch & maintenance** — launches installed games via their GOG manifest
  (with native `start.sh` / `*.exe` fallbacks), and offers `winecfg` / `regedit`
  on a game's existing prefix for Wine installs.
- **Persistent state** — install records live in `state.json`, kept separate
  from configuration so re-scanning or moving your library never loses them.

---

## Requirements

**To build**

- A C compiler with C17 support (GCC or Clang)
- [Meson](https://mesonbuild.com/) and [Ninja](https://ninja-build.org/)
- GTK 4 (`gtk4`), libadwaita 1 (`libadwaita-1`), GLib/GIO (`gio-2.0`),
  and [json-c](https://github.com/json-c/json-c) — with their development headers

**At runtime**

- `curl` and `tar` — used by the GE-Proton fetcher
- `unzip` — used when extracting certain installers
- **Wine** and/or **Proton** — only needed to install and run Windows games

See [Compiling.md](Compiling.md) for exact package names and build steps.

---

## Building & Running

```sh
meson setup build
meson compile -C build
./build/gog-manager
```

For a single portable file you can drop into your `GOG/Games` folder, grab the
latest [**release AppImage**](https://github.com/frebergguru/gog-manager/releases/latest)
(built by CI on current Arch Linux — needs a recent glibc), or build one
yourself:

```sh
./packaging/build-appimage.sh        # -> gog-manager-x86_64.AppImage
```

Releases are published automatically when a `v*` tag is pushed — e.g.
`git tag v0.1.0 && git push origin v0.1.0`.

Full details, per-distro dependencies, the AppImage packaging, and
troubleshooting are in [Compiling.md](Compiling.md).

---

## First Run

1. On first launch you'll be asked to choose your **library folder** — the
   directory that contains one subfolder per GOG product.
2. GOG Manager scans it and builds your library. Re-scan any time from the
   header bar.
3. To install a Windows game you'll need a runner. If you don't have Proton,
   open **Preferences** and fetch **GE-Proton**, or point the app at an existing
   Wine binary / Proton directory.

Expected library layout:

```
Games/
├── Some Game/
│   ├── setup_some_game_1.0.exe
│   ├── setup_some_game_1.0-1.bin
│   ├── product_1207658901.json
│   ├── icon_1207658901.png        (optional, shown on the card)
│   ├── logo_1207658901.jpg        (optional, shown on the detail page)
│   └── extras/
│       └── soundtrack.zip
└── Another Game/
    ├── product_1207658902.json
    └── another_game_en_installer.sh
```

A folder is only listed if it contains a `product_*.json`; that file supplies
the title, description, languages, supported systems and DLC links.

New installs go to `~/GOG Games/<game>` unless you set a default install
location in **Preferences**; DLC is installed into its base game's folder.

---

## Configuration & Data

By default GOG Manager scans the **current directory** for games — so you can
drop the executable (or AppImage) into your `GOG/Games` folder and just run it.
Set a fixed library folder in **Preferences** to override that; once set, it is
always used regardless of where you launch from.

The config file lives **next to the executable** so it travels with a portable
copy:

| Path | Purpose |
|------|---------|
| `<app dir>/gog-manager.cfg` | Library path (if set), install root, and configured runners |
| `<app dir>/gog-manager-state.json` | Per-game install state (keyed by product id) |
| `$XDG_DATA_HOME/gog-manager/proton/<tag>/` | Downloaded GE-Proton builds |

`<app dir>` is the directory containing the binary — for an AppImage, the folder
the `.AppImage` file sits in — so both the config and your install state travel
with a portable copy. Downloaded GE-Proton builds stay under `$XDG_DATA_HOME`
(default `~/.local/share`), since they are large and shared across setups.

---

## Project Layout

```
src/
├── main.c                 App entry point (AdwApplication)
├── game.{c,h}             GogGame model — one product and its installers/extras
├── library.{c,h}          Async scanner → GListModel<GogGame>
├── config.{c,h}           gog-manager.cfg + gog-manager-state.json load/save
├── runner.{c,h}           Wine/Proton discovery and command building
├── installer_engine.{c,h} Install/uninstall jobs (async, with progress)
├── launch.{c,h}           Launching installed games
├── proton_fetch.{c,h}     GE-Proton downloader
├── json_util.{c,h}        json-c helpers
├── text_util.{c,h}        String helpers
├── resources/             GResource: CSS and icons
└── ui/                    GTK widgets (main window, dialogs, detail view)

tests/                     Unit tests (json-util, runner command building)
```

---

## Testing

```sh
meson test -C build
```

---

## License

GOG Manager is free software, licensed under the **GNU General Public License
v3.0 or later** (GPL-3.0-or-later). See the [LICENSE](LICENSE) file for the full
text.
