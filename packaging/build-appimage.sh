#!/usr/bin/env bash
#
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Hypnotize
#
# Build a self-contained, portable AppImage of GOG Manager.
#
# The result is a single executable file — gog-manager-x86_64.AppImage — that
# bundles GTK4, libadwaita, json-c and their dependencies, plus the GSettings
# schemas, GdkPixbuf loaders and Adwaita theme that GTK loads at runtime. Drop
# it into your GOG/Games folder, `chmod +x` it, and run.
#
# It also bundles lgogdownloader, built from the newest release on GitHub each
# time this runs, so GOG checks and downloads work without installing it, and
# WebKitGTK, which shows GOG's login page inside GOG Manager.
#
# It does NOT bundle the GPU driver (Vulkan/OpenGL is always taken from the host
# system) or optional external helpers (wine, curl, tar) — those must exist on
# whatever machine runs it.
#
# Usage:
#   ./packaging/build-appimage.sh
#
# Requirements on the BUILD machine:
#   - meson, ninja, a C compiler, and the gtk4/libadwaita/json-c -dev packages
#     (same as a normal build — see Compiling.md section 2)
#   - patchelf, and one of wget/curl (to fetch the packaging tools)
#   - python3 (for the strip check, packaging/check-stripped.sh)
#   - git, CMake and lgogdownloader's build dependencies (Boost, jsoncpp,
#     RHash, tinyxml2, tidy, libcurl, zlib); see
#     https://github.com/Sude-/lgogdownloader#build-dependencies
#     Set LGOG_TAG=v3.19 (say) to build a given release instead of the newest.
#   - WebKitGTK 6.0 (webkitgtk-6.0), for the login page inside GOG Manager
#   - FUSE is NOT required; the script runs the tools in extract-and-run mode.
#
set -euo pipefail

ARCH="${ARCH:-x86_64}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" # linuxdeploy writes the AppImage into the current directory
BUILD_DIR="$ROOT/build-appimage"
APPDIR="$ROOT/AppDir"
TOOLS="$ROOT/.appimage-tools"
DESKTOP_ID="org.gogmanager.app"

# Run downloaded *.AppImage tools without needing FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1

log() { printf '\n\033[1;35m==> %s\033[0m\n' "$*"; }

fetch() {
  # fetch <url> <dest>
  local url="$1" dest="$2"
  [ -f "$dest" ] && return 0
  log "Downloading $(basename "$dest")"
  # Into a .part file first: a download cut off half-way must not leave a
  # truncated tool that the "already there" check above would reuse.
  rm -f "$dest.part"
  if command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "$dest.part" "$url"
  elif command -v curl >/dev/null 2>&1; then
    curl -fL# -o "$dest.part" "$url"
  else
    echo "error: need wget or curl to download packaging tools" >&2
    exit 1
  fi
  chmod +x "$dest.part"
  mv "$dest.part" "$dest"
}

# --- 1. Compile and stage into AppDir ------------------------------------------

log "Configuring release build"
# A release always has the login page inside GOG Manager: no WebKit, no build.
# Wiped, not reconfigured: an option added since the folder was made (like
# webkit) would be refused. The app builds in seconds.
if [ -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" --wipe --buildtype=release --prefix=/usr -Dwebkit=enabled
else
  meson setup "$BUILD_DIR" --buildtype=release --prefix=/usr -Dwebkit=enabled
fi

log "Compiling"
meson compile -C "$BUILD_DIR"

log "Staging install tree into $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" meson install -C "$BUILD_DIR" --no-rebuild --strip

# --- 1b. lgogdownloader, the newest release ----------------------------------
#
# Each AppImage carries the release that is newest when it is built
# (lgogdownloader gets a few a year). GitHub is asked at most once a day: the
# answer is kept in $TOOLS/lgogdownloader-latest and reused for 24 hours. Only
# plain vX.Y tags count; the tarball is kept in $TOOLS per version.

mkdir -p "$TOOLS"
LGOG_REPO="https://github.com/Sude-/lgogdownloader"
LGOG_LATEST="$TOOLS/lgogdownloader-latest"
if [ -z "${LGOG_TAG:-}" ]; then
  if [ -s "$LGOG_LATEST" ] && [ -n "$(find "$LGOG_LATEST" -mmin -1440)" ]; then
    LGOG_TAG="$(cat "$LGOG_LATEST")"
    log "Newest lgogdownloader release (looked up in the last day): $LGOG_TAG"
  else
    log "Looking up the newest lgogdownloader release"
    LGOG_TAG="$(git ls-remote --tags --refs "$LGOG_REPO.git" 'v*' |
      sed 's|.*refs/tags/||' | grep -E '^v[0-9]+(\.[0-9]+)*$' | sort -V | tail -n 1)"
    [ -n "$LGOG_TAG" ] && printf '%s\n' "$LGOG_TAG" > "$LGOG_LATEST"
  fi
fi
if [ -z "$LGOG_TAG" ]; then
  echo "error: could not find an lgogdownloader release at $LGOG_REPO" >&2
  exit 1
fi
LGOG_VERSION="${LGOG_TAG#v}"
LGOG_SRC="$TOOLS/lgogdownloader-$LGOG_VERSION"
fetch "$LGOG_REPO/archive/refs/tags/$LGOG_TAG.tar.gz" "$TOOLS/lgogdownloader-$LGOG_VERSION.tar.gz"
if [ ! -f "$LGOG_SRC/CMakeLists.txt" ]; then
  rm -rf "$LGOG_SRC"
  tar -xzf "$TOOLS/lgogdownloader-$LGOG_VERSION.tar.gz" -C "$TOOLS"
fi

log "Building lgogdownloader $LGOG_VERSION"
cmake -S "$LGOG_SRC" -B "$LGOG_SRC/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$LGOG_SRC/build"
DESTDIR="$APPDIR" cmake --install "$LGOG_SRC/build" --strip
# Its licence (WTFPL) travels with it.
install -Dm644 "$LGOG_SRC/COPYING" "$APPDIR/usr/share/doc/lgogdownloader/COPYING"

# --- 1c. WebKitGTK's helper programs ------------------------------------------
#
# libwebkitgtk starts its web, network and GPU processes from a folder whose
# path is written into the library (/usr/lib/webkitgtk-6.0). Inside an AppImage
# that folder is somewhere under a /tmp mount, so the path is made relative
# ("/usr" -> "././", the same length, as Tauri's AppImages do) and the AppRun
# hook below starts GOG Manager in $APPDIR/usr, where it then points.

WEBKIT_LIBDIR="$(pkg-config --variable=libdir webkitgtk-6.0)"
WEBKIT_HELPERS="$WEBKIT_LIBDIR/webkitgtk-6.0"
case "$WEBKIT_HELPERS" in
  /usr/*) ;;
  *) echo "error: WebKitGTK's helpers are not under /usr ($WEBKIT_HELPERS)" >&2; exit 1 ;;
esac
APPDIR_HELPERS="$APPDIR$WEBKIT_HELPERS"
log "Copying WebKitGTK's helper programs"
mkdir -p "$APPDIR_HELPERS/injected-bundle"
for helper in WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess; do
  [ -x "$WEBKIT_HELPERS/$helper" ] && install -m755 "$WEBKIT_HELPERS/$helper" "$APPDIR_HELPERS/"
done
install -m644 "$WEBKIT_HELPERS"/injected-bundle/*.so "$APPDIR_HELPERS/injected-bundle/"
for licence in /usr/share/licenses/webkitgtk-6.0/LICENSE /usr/share/doc/libwebkitgtk-6.0-4/copyright; do
  if [ -f "$licence" ]; then
    install -Dm644 "$licence" "$APPDIR/usr/share/doc/webkitgtk-6.0/$(basename "$licence")"
    break
  fi
done

# Started in $APPDIR/usr (see above); the folder it was started from goes on
# in GOG_MANAGER_LAUNCH_DIR, for the library and the programs GOG Manager runs.
# WebKit's sandbox (bubblewrap) can't reach the helpers inside the AppImage's
# mount; the login view only loads https pages, for GOG's login, and keeps
# nothing afterwards.
mkdir -p "$APPDIR/apprun-hooks"
cat > "$APPDIR/apprun-hooks/gog-manager-webkit.sh" <<'HOOK'
export GOG_MANAGER_LAUNCH_DIR="${GOG_MANAGER_LAUNCH_DIR:-$PWD}"
# APPDIR is the AppImage runtime's; an extracted AppDir has AppRun's folder.
cd "${APPDIR:-$this_dir}/usr" || exit 1
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
HOOK

# --- 2. Fetch packaging tools --------------------------------------------------

mkdir -p "$TOOLS"

# linuxdeploy and its gtk plugin rewrite rpaths with patchelf. Provide a static
# copy if the host doesn't have one.
if ! command -v patchelf >/dev/null 2>&1; then
  PATCHELF="$TOOLS/patchelf"
  fetch "https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0-$ARCH.tar.gz" "$TOOLS/patchelf.tar.gz"
  tar -xzf "$TOOLS/patchelf.tar.gz" -C "$TOOLS" ./bin/patchelf
  mv "$TOOLS/bin/patchelf" "$PATCHELF"
  chmod +x "$PATCHELF"
  rmdir "$TOOLS/bin" 2>/dev/null || true
fi

LD="$TOOLS/linuxdeploy-$ARCH.AppImage"
LD_GTK="$TOOLS/linuxdeploy-plugin-gtk.sh"
AT="$TOOLS/appimagetool-$ARCH.AppImage"

fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" "$LD"
fetch "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" "$LD_GTK"
fetch "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage" "$AT"

# The gtk plugin unconditionally copies the GTK 4.0 loadable-modules directory,
# but distros that ship no optional GTK4 modules (e.g. Arch) have no such dir,
# and the plugin then aborts under `set -e`. Guard that one copy so a missing
# modules dir is skipped instead of fatal. Idempotent: safe to re-run.
# Any indentation; afterwards no unguarded copy may be left.
sed -E -i 's|^([[:space:]]*)copy_lib_tree "\$gtk4_libdir" "\$APPDIR/"[[:space:]]*$|\1if [ -d "$gtk4_libdir" ]; then copy_lib_tree "$gtk4_libdir" "$APPDIR/"; else echo "No GTK 4.0 modules dir ($gtk4_libdir); skipping"; fi|' "$LD_GTK"
if grep -qE '^[[:space:]]*copy_lib_tree "\$gtk4_libdir" "\$APPDIR/"[[:space:]]*$' "$LD_GTK"; then
  echo "error: could not patch the gtk4 modules copy in $LD_GTK" >&2
  exit 1
fi

# The gtk plugin's AppRun hook forces GTK_THEME=Adwaita:<variant>. That loads
# GTK's own Adwaita theme over libadwaita's stylesheet, so preference rows lose
# their padding and groups their spacing. libadwaita follows the system's dark
# or light setting by itself, so drop the export. Idempotent: safe to re-run.
sed -E -i 's|^export GTK_THEME="\$APPIMAGE_GTK_THEME".*$|# GTK_THEME not set: it overrides libadwaita'"'"'s stylesheet|' "$LD_GTK"
if grep -qE '^export GTK_THEME=' "$LD_GTK"; then
  echo "error: could not remove the GTK_THEME export in $LD_GTK" >&2
  exit 1
fi

# linuxdeploy finds its gtk plugin and appimagetool on PATH.
export PATH="$TOOLS:$PATH"
export DEPLOY_GTK_VERSION=4
export OUTPUT="gog-manager-$ARCH.AppImage"

# --- 3. Bundle -----------------------------------------------------------------

log "Bundling dependencies"
"$LD" \
  --appdir "$APPDIR" \
  --plugin gtk \
  --executable "$APPDIR/usr/bin/lgogdownloader" \
  --deploy-deps-only "$APPDIR_HELPERS" \
  --deploy-deps-only "$APPDIR_HELPERS/injected-bundle" \
  --desktop-file "$APPDIR/usr/share/applications/$DESKTOP_ID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$DESKTOP_ID.svg"

# The helpers' path in the bundled library, made relative (see 1c). Same
# length, so nothing else in the file moves.
log "Pointing the bundled WebKitGTK at its helpers"
python3 - "$APPDIR$WEBKIT_LIBDIR"/libwebkitgtk-6.0.so.* "$WEBKIT_HELPERS" <<'PATCH'
import os, sys
*libs, helpers = sys.argv[1:]
old = helpers.encode()
new = ("././" + helpers[len("/usr"):]).encode()
assert len(old) == len(new)
patched = 0
for lib in libs:
    if os.path.islink(lib):
        continue
    data = open(lib, "rb").read()
    count = data.count(old)
    if count:
        open(lib, "wb").write(data.replace(old, new))
        patched += count
        print(f"{lib}: {count} path(s) made relative")
if not patched:
    sys.exit(f"error: {helpers} not found in {libs}")
PATCH

log "Building the AppImage"
"$AT" "$APPDIR" "$OUTPUT"

# --- 4. Refuse to release anything unstripped ---------------------------------
#
# linuxdeploy strips the libraries it bundles and `meson install --strip` the
# app binary; this checks the result rather than trusting either. The AppDir
# is exactly what was packed, and the AppImage file itself starts with the
# runtime ELF.

log "Checking that every shipped binary is stripped"
if ! "$ROOT/packaging/check-stripped.sh" "$APPDIR" "$ROOT/$OUTPUT"; then
  rm -f "$ROOT/$OUTPUT"
  echo "error: unstripped binaries in the AppImage; release refused" >&2
  exit 1
fi

log "Done: $ROOT/$OUTPUT"
echo "Copy it wherever you like (e.g. your GOG/Games folder) and run it."
