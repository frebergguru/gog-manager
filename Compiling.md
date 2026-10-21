# Compiling GOG Manager

GOG Manager is a C application built with [Meson](https://mesonbuild.com/) and
[Ninja](https://ninja-build.org/). This guide covers installing the build
dependencies, compiling, running, and installing.

---

## 1. Dependencies

### Build-time

| Component | Purpose |
|-----------|---------|
| C compiler (GCC or Clang) | Builds the sources (C17 / `gnu17`) |
| Meson + Ninja | Build system |
| `pkg-config` | Locates the libraries below |
| **GTK 4** (`gtk4`) ≥ 4.10 | UI toolkit |
| **libadwaita 1** (`libadwaita-1`) ≥ 1.6 | GNOME/Adwaita widgets |
| **GLib / GIO** (`gio-2.0`) | Core object system, async I/O |
| **json-c** | Parsing GOG `.info` manifests and config files |
| **libcurl** (`libcurl`) | GOG Galaxy cloud saves |
| `glib-compile-resources` | Bundles CSS/icons (ships with GLib) |
| *Optional:* **WebKitGTK 6.0** (`webkitgtk-6.0`) | GOG's login page inside GOG Manager; without it the login opens in the web browser. `-Dwebkit=enabled` requires it, `-Dwebkit=disabled` leaves it out |

### Runtime (optional, feature-dependent)

| Tool | Needed for |
|------|-----------|
| `curl` | Fetching GE-Proton, game categories and original languages |
| `tar` | Extracting GE-Proton |
| `unzip` | Extracting certain installers |
| `lgogdownloader` | Logging in to GOG, checking for new games and updates, downloading them and checking their files (bundled in the AppImage; built from source, the one on `PATH` is used) |
| Wine and/or Proton | Installing and running Windows games |

---

## 2. Installing dependencies

### Arch / Manjaro

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
    gtk4 libadwaita json-c curl webkitgtk-6.0
```

### Debian / Ubuntu

libadwaita 1.6 or newer is needed, so Ubuntu 24.04 and older (libadwaita 1.5)
are too old; Ubuntu 25.04 or Debian 13 work.

```sh
sudo apt install build-essential meson ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libjson-c-dev libcurl4-openssl-dev libwebkitgtk-6.0-dev
```

### Fedora

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config \
    gtk4-devel libadwaita-devel json-c-devel libcurl-devel webkitgtk6.0-devel
```

Runtime helpers, if you want the Windows-game and GE-Proton features:

```sh
# Arch/Manjaro
sudo pacman -S --needed curl tar unzip wine
# Debian/Ubuntu
sudo apt install curl tar unzip wine
# Fedora
sudo dnf install curl tar unzip wine
```

---

## 3. Configure and build

From the project root:

```sh
meson setup build
meson compile -C build
```

`meson setup build` creates an out-of-tree build directory and checks that all
dependencies are present. `meson compile -C build` builds the
`gog-manager` executable (and the test binaries).

You can also invoke Ninja directly, which is what `meson compile` calls under
the hood:

```sh
ninja -C build
```

### Debug vs. release

```sh
# Optimized release build
meson setup build --buildtype=release
# Debug build with symbols and asserts (default is 'debug')
meson setup build --buildtype=debug
```

To change options on an existing build directory, reconfigure:

```sh
meson configure build --buildtype=release
meson compile -C build
```

---

## 4. Run

```sh
./build/gog-manager
```

The executable bundles its CSS and icons as GResources, so it can be run
straight from the build directory without installing.

---

## 5. Tests

```sh
meson test -C build
```

This runs the unit tests declared in `meson.build`:

- `json-util` — JSON helpers and GOG's HTML text (descriptions, changelogs)
- `runner-cmdline` — Wine/Proton command lines and GE-Proton build handling
- `library` — library scanning, lgogdownloader output parsing, uninstall
  protection, free space, the config folder
- `play-script` — the `play.sh` written into each game's folder
- `categories` — GOG's category tags and store details
- `gamesdb` — GOG's games database details
- `shortcut` — Applications menu entries

Add `-v` for full output, or run a single suite with
`meson test -C build json-util`.

---

## 6. Install (optional)

To install system-wide (defaults under `/usr/local`):

```sh
sudo meson install -C build
```

Choose a different prefix at setup time if you prefer:

```sh
meson setup build --prefix=/usr
sudo meson install -C build
```

A system-wide install can't write next to its executable, so GOG Manager then
keeps its config files in `~/.config/gog-manager/` (`$XDG_CONFIG_HOME/gog-manager`)
instead of a `.gog-manager-configs` folder beside the binary. The install also
puts GOG Manager's license and the LGPL-3.0 text for its Adwaita icons in
`share/licenses/gog-manager/`.

---

## 7. Portable build (AppImage)

If you want a single, self-contained file you can drop into your `GOG/Games`
folder (or copy to another machine) rather than a system install, build an
**AppImage**:

```sh
./packaging/build-appimage.sh
```

This produces `gog-manager-x86_64.AppImage` in the project root. Make it
executable and run it directly:

```sh
chmod +x gog-manager-x86_64.AppImage
./gog-manager-x86_64.AppImage
```

### Why not a fully static binary?

A plain statically-linked executable is not viable for a GTK 4 / libadwaita
app. GTK loads image loaders and I/O modules dynamically at runtime, needs its
compiled GSettings schemas present on disk, and the GPU driver (Vulkan/OpenGL)
must always come from the host system. The AppImage is the working equivalent:
it bundles GTK 4, libadwaita, json-c, WebKitGTK 6.0 (with its helper programs)
and lgogdownloader with all their dependent libraries, the compiled GSettings
schemas, the GdkPixbuf loaders and the Adwaita theme into one file, with the
binary's runpath pointed at the bundled libraries. Only the GPU
driver and optional external helpers (wine, curl, tar) are taken from the host.

### How the script works

1. Configures a `--buildtype=release` build and stages it into an `AppDir` via
   `DESTDIR` install (this is why `meson.build` installs the `.desktop` file and
   icon from `packaging/`).
2. Downloads `linuxdeploy`, its GTK plugin and `appimagetool` into
   `.appimage-tools/` (cached across runs), plus a static `patchelf` if the host
   lacks one.
3. Patches the GTK plugin: a missing GTK 4 modules directory is skipped
   instead of being fatal, and its AppRun hook no longer sets `GTK_THEME`.
   That variable loads GTK's own Adwaita theme over libadwaita's stylesheet
   and drops its padding and spacing; libadwaita follows the desktop's dark or
   light setting by itself. (So `APPIMAGE_GTK_THEME` has no effect either.)
4. Builds lgogdownloader from its newest release (the tag is looked up with
   `git ls-remote` at most once a day and kept in
   `.appimage-tools/lgogdownloader-latest`; set `LGOG_TAG` to pick one) and
   installs it, stripped, into the AppDir.
5. Copies WebKitGTK's web and network processes (the build stops if either is
   missing), the GPU process when the WebKit build has one, and the injected
   bundle into the AppDir, and adds an AppRun hook that starts GOG
   Manager in `$APPDIR/usr` (keeping the folder it was started from in
   `GOG_MANAGER_LAUNCH_DIR`) with WebKit's sandbox off.
6. Runs `linuxdeploy --plugin gtk` to bundle the libraries of GOG Manager,
   lgogdownloader and WebKit's helpers, then rewrites the helpers' path in the
   bundled `libwebkitgtk-6.0.so` to a relative one of the same length
   (`/usr/lib/webkitgtk-6.0` → `././/lib/webkitgtk-6.0`), and drops the
   GObject introspection typelibs the GTK plugin copies (GOG Manager is C and
   never loads them).
7. Runs `packaging/collect-licenses.py`, which traces every bundled file to
   the build machine's package (pacman, dpkg or rpm), copies each package's
   license texts to `usr/share/licenses/<package>/` and lists its version,
   license and source address in
   `usr/share/doc/gog-manager/THIRD-PARTY-NOTICES.txt`. It fails the build if
   a bundled file has no package or a package has no license text. Then the
   AppDir is packed with `appimagetool`.
8. Runs `packaging/check-stripped.sh` on the AppDir and the finished AppImage,
   and deletes the AppImage and fails if any binary or bundled library still
   has symbols or debug info.

Build-machine requirements: the normal build dependencies (section 2) with
WebKitGTK 6.0, lgogdownloader's build dependencies (CMake, Boost, jsoncpp,
RHash, tinyxml2, tidy, libcurl, zlib; see its README), plus `git`, `python3`
(for the strip check, the license collection and the WebKit patch), `pacman`,
`dpkg` or `rpm` (to find each bundled library's package and license),
`patchelf` (downloaded if missing), and `wget` or `curl` for the tool
downloads. On Arch, the `licenses` package provides the common license texts
some packages refer to. FUSE is **not** required — the
script runs the tools in extract-and-run mode.

Set `ARCH` to target a different architecture (default `x86_64`). Delete
`.appimage-tools/` to force a fresh download of the packaging tools.

---

## 8. Troubleshooting

**`meson setup` fails with "Dependency ... not found"**
A `-dev` / `-devel` package is missing. Re-check the dependency list in
section 2 — the error names the missing `pkg-config` module (e.g. `gtk4`,
`libadwaita-1`, `json-c`).

**`libadwaita-1` not found on older distros**
GOG Manager needs a reasonably recent GTK 4 and libadwaita. On an older
release you may need to enable a backports repository or update your system to
get current versions.

**Clean rebuild**
If a build gets into a bad state, delete the build directory and start over:

```sh
rm -rf build
meson setup build
meson compile -C build
```

**The login opens in the web browser instead of inside GOG Manager**
GOG Manager was built without WebKitGTK 6.0. Install its development package
(section 2) and reconfigure with `meson setup build --reconfigure -Dwebkit=enabled`.

**Runtime: icons show as missing / broken boxes**
The app's own button icons are built into it, but the view-switcher icons and
the app icon in the About window come from the system. Installing the Adwaita
icon theme (usually pulled in by GTK 4) and the `hicolor-icon-theme` package,
or installing GOG Manager with `meson install`, resolves it.

**Runtime: Windows game won't install or launch**
Make sure a runner is available — install Wine, or fetch GE-Proton from the
app's Preferences. Verify the runner path in Preferences points at a valid
Wine binary or Proton directory.
