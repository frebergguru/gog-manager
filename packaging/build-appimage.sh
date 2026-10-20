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
if [ -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" --reconfigure --buildtype=release --prefix=/usr
else
  meson setup "$BUILD_DIR" --buildtype=release --prefix=/usr
fi

log "Compiling"
meson compile -C "$BUILD_DIR"

log "Staging install tree into $APPDIR"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" meson install -C "$BUILD_DIR" --no-rebuild --strip

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

log "Bundling dependencies and building AppImage"
"$LD" \
  --appdir "$APPDIR" \
  --plugin gtk \
  --desktop-file "$APPDIR/usr/share/applications/$DESKTOP_ID.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/$DESKTOP_ID.svg" \
  --output appimage

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
