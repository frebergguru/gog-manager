# GOG Manager

[![AppImage](https://github.com/frebergguru/gog-manager/actions/workflows/appimage.yml/badge.svg)](https://github.com/frebergguru/gog-manager/actions/workflows/appimage.yml)

A native GTK4 / libadwaita application for Linux that turns a folder of GOG
**offline installers** into a browsable library you can install, launch, and
manage — including Windows-only titles, which it runs transparently through
Wine or Proton.

You point it at a directory of installers you already own (the `.exe`/`.bin`
files and their `extras/`), and it does the rest locally. GOG Manager never
holds your GOG login or password itself: to check for new games and updates,
download them and check their files, it runs
[lgogdownloader](https://github.com/Sude-/lgogdownloader), which keeps the
login. GOG Manager can show GOG's login page for it.

---

## Features

- **Library browser** — scans a directory of GOG products (one subfolder per
  game), reads each product's `product_*.json` metadata, and presents titles,
  logos, descriptions, and sizes in an Adwaita grid.
- **Search, filter, and sort** — live search by title, series, developer or
  publisher; filter by OS (Windows / Linux / macOS), by language, by games
  with DLC or extras, and by status (installed, not installed, new version on
  GOG); sort by title (A–Z / Z–A), installed-first, or size.
- **New games and updates from GOG** — through
  [lgogdownloader](https://github.com/Sude-/lgogdownloader), lists the games
  you own that are not downloaded yet and the ones with a new version. Each
  game opens into separate **Game**, **DLC** and **Extras** groups, each
  listing its files with name, size and language, so you can see what is what
  and pick exactly what to download, and you can choose which
  **languages** to download per game (remembered). The download shows the
  **total progress** and a progress bar for **each file** being downloaded, and
  you choose how many files download **at the same time** (1–8). Once
  everything has downloaded, old versions are deleted. If a file fails (for
  example HTTP 404 or 500, or a network error), it says which file and why,
  keeps the game's old version, and asks whether to try again later or ignore
  the file (until GOG puts out a new version of it). **Quick Check** looks
  only at the games GOG flags as new or updated, which takes seconds. The
  download settings also have a speed limit, "Include games hidden on GOG"
  and "Include patches" (all off by default), and a full check lists your GOG
  notifications. Before downloading, it checks there is enough free space for
  the files it will actually fetch.
- **Log in from GOG Manager** — **Log In to GOG…** in the download window
  shows GOG's login page inside GOG Manager (WebKitGTK; https only, nothing
  kept afterwards) and hands the result to lgogdownloader, so there is nothing
  to type in a terminal; without WebKit it opens in the web browser and you
  paste the address. The button shows when lgogdownloader is logged in.
- **Check Files** — a downloaded game's page can compare its files with GOG's
  checksums (lgogdownloader `--repair`) and download damaged parts again.
- **Download languages** — by default each game's original language (looked
  up on Wikidata) plus the languages ticked under **Also Download** in
  Preferences; each game can have its own choice.
- **Games not downloaded yet** — the games you own on GOG but haven't
  downloaded are in the library as well (after a check in the download
  window), with their box art and details and a **Download…** button.
- **One-click install** — installs Linux-native games directly, and drives
  Windows installers unattended through Wine or Proton, laying down a portable
  `c:\game` prefix that keeps game files outside the Wine prefix.
- **Wine / Proton runners** — auto-discovers system Wine and any Proton builds
  under Steam's `compatibilitytools.d` and Heroic's tool directories, plus
  custom runner paths you add in Preferences.
- **Built-in GE-Proton fetcher** — downloads and extracts the latest
  [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom) release into
  a local tools directory, with live progress; **Manage…** in Preferences
  lists the downloaded builds with their sizes and removes the ones you pick,
  or all but the newest.
- **DLC & extras** — links DLC products to their base game and installs them
  into the base game's directory; surfaces bonus `extras/` content
  (manuals, soundtracks, wallpapers) per game.
- **Serial keys and GOG's messages** — shows the keys from `serials.txt` (or,
  without one, the CD keys in lgogdownloader's `game-details.json`) on the
  game's page, with a copy button, and GOG's messages about the game at the
  top of its About tab.
- **Launch & maintenance** — launches installed games via their GOG manifest
  (with native `start.sh` / `*.exe` fallbacks), uninstalls them (never
  deleting the library, the install root or another game's folder), checks
  for free space before installing, and offers `winecfg` / `regedit`
  on a game's existing prefix for Wine installs. Every installed game also
  gets a `play.sh` in its folder that starts it the same way without
  gog-manager (or the library disk) — kept up to date when the runner changes.
- **Categories** — filter by one or several of GOG's genres (any or all of
  them) and sort by the main one, which each card shows. gog-manager fetches
  them from GOG once per game in the background after a scan and keeps them
  in `gog-manager-categories.json` in the app's config folder, so they work offline;
  Preferences can fetch them all again.
- **Box art and details** — from GOG's games database: cards show each
  game's box art, and its page the background art, genres, developer,
  publisher, series, themes, game modes, critics' rating and first release
  date. A **Screenshots** tab shows the screenshots and artwork (click or
  press Enter for full size, then Left/Right for the next) and a **Videos**
  tab links the trailers on YouTube. Filter by genre, theme, game mode,
  series, developer or publisher (several at once, each list searchable), and
  sort by rating, series (in release order) or developer. Cached in the
  config folder like the categories; screenshots are fetched the first time
  a game's Screenshots tab is opened. The page updates when the details
  arrive while it is open.
- **Store details** — from GOG's store API and the product JSON: features
  (cloud saves, controller support...), GOG's tags, audio and text
  languages, age ratings, editions, DOSBox, a **Requirements** tab, a
  **Changelog** tab, and buttons for the game's store page, support page and
  forum. Filter by feature or tag.
- **Views** — show the library as a grid of covers, a simple list or a
  detailed table (categories, platforms, status, size, release date,
  languages, DLC and extras, genres, developer, series, rating); the choice
  is remembered.
- **Release dates** — sort the library newest or oldest first.
- **Newest Proton** — the default runner for new installs is the newest
  Proton on the computer (by the version each build records, so Heroic's
  older "GE-Proton-latest" folder isn't mistaken for it); a specific runner
  can still be chosen in Preferences. An installed game keeps its runner.
- **Applications menu** — an installed game's page can add it to the
  Applications menu; the entry starts its `play.sh`, so it runs without
  gog-manager or the library disk.
- **Persistent state** — install records live in `gog-manager-state.json`, kept separate
  from configuration so re-scanning or moving your library never loses them.

---

## Requirements

**To build**

- A C compiler with C17 support (GCC or Clang)
- [Meson](https://mesonbuild.com/) and [Ninja](https://ninja-build.org/)
- GTK 4 ≥ 4.10 (`gtk4`), libadwaita ≥ 1.6 (`libadwaita-1`), GLib/GIO (`gio-2.0`),
  and [json-c](https://github.com/json-c/json-c) — with their development headers
- Optional: WebKitGTK 6.0 (`webkitgtk-6.0`), to show GOG's login page inside
  GOG Manager (`-Dwebkit=enabled` to require it; the AppImage always has it)

**At runtime**

- `curl` — used by the GE-Proton fetcher and to look up game categories and
  original languages
- `tar` — used by the GE-Proton fetcher
- `unzip` — used when extracting certain installers
- [lgogdownloader](https://github.com/Sude-/lgogdownloader) — only needed to
  check GOG for new games and updates, download them and check their files.
  See [Setting up lgogdownloader](#setting-up-lgogdownloader). GOG Manager
  checks whether it is installed and, if it isn't, says how to get it instead
  of offering the check.
- **Wine** and/or **Proton** — only needed to install and run Windows games

See [Compiling.md](Compiling.md) for exact package names and build steps.

---

## Setting up lgogdownloader

GOG Manager talks to GOG through
[lgogdownloader](https://github.com/Sude-/lgogdownloader): it holds your GOG
login, and GOG Manager runs it for the check, downloads and file checks.

**With the AppImage there is nothing to install:** it comes with lgogdownloader,
the newest release when the AppImage was built. Skip to
[logging in](#2-log-in). The AppImage uses its own copy even when your system
has one too.

Built from source, GOG Manager uses the lgogdownloader on your `PATH`. It is
tested with lgogdownloader 3.19; older versions may lack some of the options it
uses, so take the newest your distribution offers.

### 1. Install it (not needed with the AppImage)

**Debian** — in the official archive. Debian 13 "trixie" has 3.17:

```sh
sudo apt install lgogdownloader
```

On Debian 12 "bookworm" the regular package is 3.9; take 3.15 from
[backports](https://backports.debian.org/Instructions/) instead:

```sh
sudo apt install -t bookworm-backports lgogdownloader
```

**Ubuntu**, Linux Mint, Pop!_OS and other Ubuntu-based distributions — in the
`universe` component (Ubuntu 26.04 has 3.18, 24.04 has 3.12):

```sh
sudo add-apt-repository universe
sudo apt update
sudo apt install lgogdownloader
```

**Arch Linux**, Manjaro, EndeavourOS — not in the official repositories, but in
the [AUR](https://aur.archlinux.org/packages/lgogdownloader). With an AUR helper:

```sh
yay -S lgogdownloader        # or: paru -S lgogdownloader
pamac build lgogdownloader   # Manjaro's own tool
```

**Fedora** — in the official repositories (Fedora 43 and newer):

```sh
sudo dnf install lgogdownloader
```

**Red Hat Enterprise Linux**, AlmaLinux, Rocky Linux, CentOS Stream — there is
no package, not in EPEL either. Build it from source as below.

**From source** (any distribution): install the build dependencies listed in
[lgogdownloader's README](https://github.com/Sude-/lgogdownloader#build-dependencies)
for your distribution (CMake, Ninja, libcurl, Boost, jsoncpp, RHash, tinyxml2,
tidy, zlib), then:

```sh
git clone https://github.com/Sude-/lgogdownloader.git
cd lgogdownloader
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -GNinja
ninja -C build
sudo ninja -C build install
```

Check that it runs: `lgogdownloader --version`.

### 2. Log in

**In GOG Manager:** open **New Games and Updates on GOG** (the update button in
the header bar) and press **Log In to GOG…**. GOG's login page opens right
there; log in as on GOG's website (e-mail and password, or Google, Steam,
Discord or Xbox), and GOG Manager finishes the login by itself. GOG Manager
never sees your password, and the page keeps nothing once it is closed
(lgogdownloader keeps the login).

Built from source without WebKitGTK 6.0, or with **Use My Web Browser
Instead**, the page opens in your web browser instead; then copy the address
of the page it ends on (it starts with `https://embed.gog.com/on_login_success`)
and paste it into GOG Manager.

**Or in a terminal**, once:

```sh
lgogdownloader --login
```

It asks for your GOG e-mail address and password, and saves the login in
`~/.config/lgogdownloader/` (it stays logged in from then on; GOG Manager never
sees your password). If GOG asks for more than that (two-step verification, a
captcha) and the login fails, use the browser login instead (lgogdownloader
3.15 and newer) and follow what it prints:

```sh
lgogdownloader --browser-login
```

Check it worked:

```sh
lgogdownloader --check-login-status
```

It should say `Login status: Logged in`. If GOG Manager later says
lgogdownloader is not logged in (GOG ends logins now and then), run
`lgogdownloader --login` again.

### 3. Point GOG Manager at your games

GOG Manager tells lgogdownloader where to download (`--directory`): the library
folder chosen in GOG Manager, with one folder per game named as lgogdownloader
names it. If you already download with lgogdownloader, choose that folder as
the library. Platforms, and other settings GOG Manager doesn't set itself, come
from lgogdownloader's `~/.config/lgogdownloader/config.cfg`.

Then open **New Games and Updates on GOG** in GOG Manager (the update button
in the header bar) and press **Check Now**.

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
`git tag vX.Y.Z && git push origin vX.Y.Z`.

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
│   ├── serials.txt                (optional, CD keys shown on the detail page)
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

The config files live in a hidden `.gog-manager-configs` folder **next to the
executable**, so they travel with a portable copy. When that folder can't be
written to (for example a system install in `/usr/bin`), they go to
`$XDG_CONFIG_HOME/gog-manager/` (default `~/.config/gog-manager/`) instead.
Files an older version kept directly next to the executable are moved into
`.gog-manager-configs` on start:

| Path | Purpose |
|------|---------|
| `<config dir>/gog-manager.cfg` | Library path (if set), install root, and configured runners |
| `<config dir>/gog-manager-state.json` | Per-game install state (keyed by product id) |
| `<config dir>/gog-manager-updates.json` | The last GOG check: what each game has to download |
| `<config dir>/gog-manager-download.json` | Download choices: files at the same time, speed limit, hidden games, patches, each game's languages; GOG's notifications from the last check |
| `<config dir>/gog-manager-categories.json` | GOG's store details per game: categories, features, tags, languages, requirements, age ratings, editions |
| `<config dir>/gog-manager-original-languages.json` | Each game's original language (from Wikidata), for the default download languages |
| `<config dir>/gog-manager-gamesdb.json` | Details from GOG's games database (summary, genres, developers, themes, rating, screenshot and video links...) |
| `<config dir>/gamesdb-images/` | Box art, backgrounds and the screenshots you have looked at, from GOG's games database |
| `$XDG_DATA_HOME/gog-manager/proton/<tag>/` | Downloaded GE-Proton builds |
| `$XDG_DATA_HOME/applications/gog-manager-<id>.desktop` | Applications menu entries you added |
| `$XDG_DATA_HOME/gog-manager/icons/` | Their icons |
| `$XDG_DATA_HOME/gog-manager/steam-compat-client/` | Empty folder Proton needs as its Steam client path |

The GOG check and downloads use lgogdownloader's own settings in
`~/.config/lgogdownloader/config.cfg` (platforms and so on); the library folder
should be lgogdownloader's download directory, with one `<gamename>` folder per
game.

`<config dir>` is `.gog-manager-configs` in the folder containing the binary —
for an AppImage, the folder the `.AppImage` file sits in — or
`~/.config/gog-manager` when that folder is read-only. Downloaded GE-Proton builds stay under `$XDG_DATA_HOME`
(default `~/.local/share`), since they are large and shared across setups.

---

## Project Layout

```
src/
├── main.c                 App entry point (AdwApplication)
├── game.{c,h}             GogGame model — one product and its installers/extras
├── library.{c,h}          Async scanner → GListModel<GogGame>
├── config.{c,h}           Config folder, gog-manager.cfg + gog-manager-state.json
├── runner.{c,h}           Wine/Proton discovery and command building
├── installer_engine.{c,h} Install/uninstall jobs (async, with progress)
├── launch.{c,h}           Launching installed games, play.sh
├── shortcut.{c,h}         Applications menu entries
├── proton_fetch.{c,h}     GE-Proton downloader and build management
├── gog_sync.{c,h}         GOG checks, downloads, login and file checks through lgogdownloader
├── categories.{c,h}       GOG's store API (api.gog.com/v2/games): categories and store details
├── gamesdb.{c,h}          GOG's games database: box art, details, screenshots, videos
├── original_language.{c,h} Each game's original language, from Wikidata
├── fetch.{c,h}            Parallel downloads through curl
├── json_util.{c,h}        json-c helpers
├── text_util.{c,h}        String and GOG HTML helpers
├── resources/             GResource: CSS and icons
└── ui/
    ├── main_window.{c,h}          Library window: grid/list/table, filters, sorting
    ├── game_detail.{c,h}          Game page and its tabs
    ├── install_dialog.{c,h}       Install window
    ├── gog_sync_dialog.{c,h}      New Games and Updates on GOG (download window)
    ├── login_dialog.{c,h}         Log In to GOG (WebKit page or web browser)
    ├── repair_dialog.{c,h}        Check Files (lgogdownloader --repair)
    ├── preferences_dialog.{c,h}   Preferences (Library, Runners, Downloads)
    ├── proton_fetch_dialog.{c,h}  GE-Proton download
    ├── proton_manage_dialog.{c,h} Manage and remove GE-Proton builds
    └── dialog_window.{c,h}        Shared resizable dialog window and alerts

tests/                     Unit tests (json-util and GOG's HTML text, runner
                           command building and GE-Proton, library scanning,
                           lgogdownloader output parsing, uninstall protection,
                           config folder, play.sh, categories and store
                           details, games database, menu entries)

packaging/
├── build-appimage.sh      Builds the AppImage (bundles lgogdownloader and WebKitGTK)
├── check-stripped.sh      Fails the release if any shipped binary is unstripped
├── collect-licenses.py    Ships every bundled library's license; fails the release if one has none
├── org.gogmanager.app.desktop
└── org.gogmanager.app.svg

LICENSES/
└── LGPL-3.0-only.txt      The license the Adwaita icons are used under

meson.build, meson_options.txt (option `webkit`), .github/workflows/appimage.yml (CI)
```

---

## Testing

```sh
meson test -C build
```

---

## Credits

- [lgogdownloader](https://github.com/Sude-/lgogdownloader) by Sude- and
  contributors does the GOG side: login, checks and downloads. GOG Manager runs
  it as a separate program, and `src/gog_sync.c` borrows from it: its table of
  languages and their bit values, and its file type values, come from
  lgogdownloader's `include/globalconstants.h`, and the parsing follows the
  output formats in its source. lgogdownloader is released under the
  [WTFPL](https://github.com/Sude-/lgogdownloader/blob/master/COPYING).
- [GE-Proton](https://github.com/GloriousEggroll/proton-ge-custom) by
  GloriousEggroll, which the GE-Proton fetcher downloads.
- The symbolic icons in `src/resources/icons` come from the
  [Adwaita icon theme](https://gitlab.gnome.org/GNOME/adwaita-icon-theme) by the
  GNOME Project, released under CC-BY-SA-3.0 or LGPL-3.0-only (used here under
  LGPL-3.0, see [LICENSES/LGPL-3.0-only.txt](LICENSES/LGPL-3.0-only.txt)).
  They are built into the app so every button has its icon whatever icon theme
  the desktop uses.

The AppImage also bundles GTK, libadwaita, GLib, WebKitGTK, json-c,
lgogdownloader's libraries (Boost under BSL-1.0, curl, jsoncpp, tinyxml2,
RHash, tidy) and their dependencies, each under its own license (mostly
LGPL-2.1-or-later, MIT and BSD). Their license texts, versions and source
addresses are inside the AppImage, in `usr/share/licenses/<package>/` and
`usr/share/doc/gog-manager/THIRD-PARTY-NOTICES.txt`; the About window says so
too.

---

## License

GOG Manager is free software, licensed under the **GNU General Public License
v3.0 or later** (GPL-3.0-or-later). See the [LICENSE](LICENSE) file for the full
text. An install puts it, and the LGPL-3.0 text for the Adwaita icons, in
`share/licenses/gog-manager/`.
