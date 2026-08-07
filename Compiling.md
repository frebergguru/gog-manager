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
| **GTK 4** (`gtk4`) | UI toolkit |
| **libadwaita 1** (`libadwaita-1`) | GNOME/Adwaita widgets |
| **GLib / GIO** (`gio-2.0`) | Core object system, async I/O |
| **json-c** | Parsing GOG `.info` manifests and config files |
| `glib-compile-resources` | Bundles CSS/icons (ships with GLib) |

### Runtime (optional, feature-dependent)

| Tool | Needed for |
|------|-----------|
| `curl`, `tar` | Fetching and extracting GE-Proton |
| `unzip` | Extracting certain installers |
| Wine and/or Proton | Installing and running Windows games |

---

## 2. Installing dependencies

### Arch / Manjaro

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf \
    gtk4 libadwaita json-c
```

### Debian / Ubuntu

```sh
sudo apt install build-essential meson ninja-build pkg-config \
    libgtk-4-dev libadwaita-1-dev libjson-c-dev
```

### Fedora

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config \
    gtk4-devel libadwaita-devel json-c-devel
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

- `json-util` — JSON parsing/helpers
- `runner-cmdline` — Wine/Proton command-line construction

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
it bundles GTK 4, libadwaita, json-c and their ~130 dependent libraries, the
compiled GSettings schemas, the GdkPixbuf loaders and the Adwaita theme into one
file, with the binary's runpath pointed at the bundled libraries. Only the GPU
driver and optional external helpers (wine, curl, tar) are taken from the host.

### How the script works

1. Configures a `--buildtype=release` build and stages it into an `AppDir` via
   `DESTDIR` install (this is why `meson.build` installs the `.desktop` file and
   icon under `packaging/`).
2. Downloads `linuxdeploy`, its GTK plugin and `appimagetool` into
   `.appimage-tools/` (cached across runs), plus a static `patchelf` if the host
   lacks one.
3. Runs `linuxdeploy --plugin gtk --output appimage` to bundle everything.

Build-machine requirements: the normal build dependencies (section 2), plus
`wget` or `curl` for the one-time tool download. FUSE is **not** required — the
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

**Runtime: icons show as missing / broken boxes**
This depends on your desktop's icon theme providing the standard symbolic
icons. Installing the Adwaita icon theme (usually pulled in by GTK 4) or the
`hicolor-icon-theme` package resolves it.

**Runtime: Windows game won't install or launch**
Make sure a runner is available — install Wine, or fetch GE-Proton from the
app's Preferences. Verify the runner path in Preferences points at a valid
Wine binary or Proton directory.
