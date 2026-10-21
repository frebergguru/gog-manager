#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Hypnotize
#
# Ships the license of everything the AppImage bundles from the build
# machine's packages.
#
# Usage: collect-licenses.py <AppDir>
#
# Every bundled file that isn't GOG Manager's or lgogdownloader's own is
# traced back to the system package it came from (pacman, dpkg or rpm). For
# each package its license texts are copied into
# <AppDir>/usr/share/licenses/<package>/, and
# <AppDir>/usr/share/doc/gog-manager/THIRD-PARTY-NOTICES.txt lists the
# package, its version, its license and where its source is.
#
# Exits 1, so the release stops, when a bundled file has no package or a
# package has no license text to ship.

import os
import re
import shutil
import subprocess
import sys

# Our own files, and files linuxdeploy generates on the build machine.
OWN = {
    "usr/bin/gog-manager",
    "usr/bin/lgogdownloader",
    "usr/share/applications/org.gogmanager.app.desktop",
    "usr/share/icons/hicolor/scalable/apps/org.gogmanager.app.svg",
    "usr/share/man/man1/lgogdownloader.1",
    "usr/share/man/man1/lgogdownloader.1.gz",
    "usr/share/glib-2.0/schemas/gschemas.compiled",
    "usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache",
}
OWN_PREFIXES = ("usr/share/licenses/", "usr/share/doc/", "apprun-hooks/")
HOST_LIB_DIRS = ("/usr/lib", "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu")


def run(*argv):
    return subprocess.run(argv, capture_output=True, text=True).stdout


def bundled_files(appdir):
    for dirpath, _, names in os.walk(appdir):
        for name in names:
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, appdir)
            if os.path.dirname(rel) == "" or rel in OWN or rel.startswith(OWN_PREFIXES):
                continue  # AppRun, the .desktop file, .DirIcon...
            if os.path.isfile(path):
                yield rel


def host_index():
    """Basename -> host paths, for libraries linuxdeploy flattened into usr/lib."""
    index = {}
    for top in HOST_LIB_DIRS:
        if not os.path.isdir(top) or os.path.islink(top):
            continue
        for dirpath, _, names in os.walk(top):
            for name in names:
                index.setdefault(name, []).append(os.path.join(dirpath, name))
    return index


def host_path(rel, index):
    """Where a bundled file came from on the build machine."""
    if rel.startswith("usr/lib/"):
        sub = rel[len("usr/lib/"):]
        for top in HOST_LIB_DIRS:
            candidate = os.path.join(top, sub)
            if os.path.exists(candidate):
                return candidate
        found = index.get(os.path.basename(rel))
        return found[0] if found else None
    candidate = "/" + rel
    return candidate if os.path.exists(candidate) else None


# --- package managers ---------------------------------------------------------


class Pacman:
    name = "pacman"

    def owners(self, paths):
        out = {}
        for path in paths:
            pkg = run("pacman", "-Qoq", path).strip()
            if pkg:
                out[path] = pkg
        return out

    def info(self, pkg):
        fields = {}
        for line in run("pacman", "-Qi", pkg).splitlines():
            key, sep, value = line.partition(":")
            if sep and not line.startswith(" "):
                fields[key.strip()] = value.strip()
        version = fields.get("Version", "")
        base = pkg
        local = f"/var/lib/pacman/local/{pkg}-{version}/desc"
        try:
            lines = open(local).read().splitlines()
            if "%BASE%" in lines:
                base = lines[lines.index("%BASE%") + 1]
        except OSError:
            pass
        tag = version.replace(":", "-")
        # The build recipe at that release: the upstream source's address,
        # its checksum and Arch's patches.
        source = f"https://gitlab.archlinux.org/archlinux/packaging/packages/{base}/-/tree/{tag}"
        return version, fields.get("Licenses", ""), source

    def license_files(self, pkg, licenses):
        files = []
        own = f"/usr/share/licenses/{pkg}"
        if os.path.isdir(own):
            for dirpath, _, names in os.walk(own):
                # isfile follows links: a link to a text the system doesn't
                # have (a slim container) is left out.
                files += [os.path.join(dirpath, n) for n in sorted(names)
                          if os.path.isfile(os.path.join(dirpath, n))]
        # Arch's common licenses aren't copied into each package: the
        # SPDX texts come from the licenses package.
        for spdx_id in spdx_ids(licenses):
            text = f"/usr/share/licenses/spdx/{spdx_id}.txt"
            if os.path.isfile(text) and text not in files:
                files.append(text)
        return files


class Dpkg:
    name = "dpkg"

    def owners(self, paths):
        out = {}
        for path in paths:
            line = run("dpkg", "-S", path).splitlines()
            if line and ":" in line[0]:
                out[path] = line[0].split(":")[0].split(",")[0].strip()
        return out

    def info(self, pkg):
        version, source, source_version = run(
            "dpkg-query", "-W", "-f", "${Version}\t${source:Package}\t${source:Version}", pkg
        ).split("\t")
        return version, "see copyright", f"https://sources.debian.org/src/{source}/{source_version}/"

    def license_files(self, pkg, licenses):
        copyright = f"/usr/share/doc/{pkg.split(':')[0]}/copyright"
        return [copyright] if os.path.isfile(copyright) else []


class Rpm:
    name = "rpm"

    def owners(self, paths):
        out = {}
        for path in paths:
            pkg = run("rpm", "-qf", "--qf", "%{NAME}\n", path).splitlines()
            if pkg and "not owned" not in pkg[0]:
                out[path] = pkg[0]
        return out

    def info(self, pkg):
        version, licenses, srpm = run(
            "rpm", "-q", "--qf", "%{VERSION}-%{RELEASE}\t%{LICENSE}\t%{SOURCERPM}", pkg
        ).split("\t")
        src_name = re.sub(r"-[^-]+-[^-]+\.src\.rpm$", "", srpm)
        return version, licenses, f"https://src.fedoraproject.org/rpms/{src_name} ({srpm})"

    def license_files(self, pkg, licenses):
        return [f for f in run("rpm", "-qL", pkg).splitlines() if os.path.isfile(f)]


# Arch's names from before it moved to SPDX ids, as some packages still say.
LEGACY = {
    "GPL": "GPL-2.0-or-later", "GPL2": "GPL-2.0-only", "GPL3": "GPL-3.0-only",
    "LGPL": "LGPL-2.1-or-later", "LGPL2": "LGPL-2.0-only", "LGPL2.1": "LGPL-2.1-only",
    "LGPL3": "LGPL-3.0-only", "MPL": "MPL-1.1", "MPL2": "MPL-2.0", "Apache": "Apache-2.0",
    "APACHE": "Apache-2.0", "FDL": "GFDL-1.3-or-later",
}


def spdx_ids(expression):
    ids = re.findall(r"[A-Za-z0-9.+-]+", expression)
    return [LEGACY.get(i, i) for i in ids
            if i not in ("AND", "OR", "WITH") and not i.startswith("LicenseRef")]


def package_manager():
    for cls, tool in ((Pacman, "pacman"), (Dpkg, "dpkg-query"), (Rpm, "rpm")):
        if shutil.which(tool):
            return cls()
    sys.exit("error: no pacman, dpkg or rpm here to look up the bundled packages' licenses")


# --- main ----------------------------------------------------------------------


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__ or "usage: collect-licenses.py <AppDir>")
    appdir = sys.argv[1]
    pm = package_manager()
    index = host_index()

    unknown = []
    host = {}
    for rel in bundled_files(appdir):
        path = host_path(rel, index)
        if path:
            host[rel] = path
        else:
            unknown.append(rel)
    owners = pm.owners(sorted(set(host.values())))
    packages = {}
    for rel, path in sorted(host.items()):
        pkg = owners.get(path)
        if pkg:
            packages.setdefault(pkg, []).append(rel)
        else:
            unknown.append(rel)

    if unknown:
        for rel in sorted(unknown):
            print(f"error: no {pm.name} package found for bundled {rel}", file=sys.stderr)
        return 1

    licenses_dir = os.path.join(appdir, "usr/share/licenses")
    notices = []
    missing = []
    for pkg in sorted(packages):
        version, licenses, source = pm.info(pkg)
        files = pm.license_files(pkg, licenses)
        if not files:
            missing.append(f"{pkg} ({licenses or 'no license field'})")
            continue
        dest = os.path.join(licenses_dir, pkg)
        os.makedirs(dest, exist_ok=True)
        for f in files:
            shutil.copy(f, os.path.join(dest, os.path.basename(f)))
        notices.append((pkg, version, licenses, source, len(packages[pkg])))

    if missing:
        for m in missing:
            print(f"error: no license text to ship for {m}", file=sys.stderr)
        return 1

    doc = os.path.join(appdir, "usr/share/doc/gog-manager")
    os.makedirs(doc, exist_ok=True)
    with open(os.path.join(doc, "THIRD-PARTY-NOTICES.txt"), "w") as out:
        out.write(
            "GOG Manager is free software under the GNU General Public License v3.0\n"
            "or later; its license is in usr/share/licenses/gog-manager/LICENSE and its\n"
            "source at https://github.com/frebergguru/gog-manager.\n\n"
            "This AppImage also contains the third-party software below, each under\n"
            "its own license. Each package's license texts are in\n"
            "usr/share/licenses/<package>/. The address listed with each package is\n"
            "its build recipe at the version shipped, which names its upstream source\n"
            "and that source's checksum.\n\n"
            "lgogdownloader\n"
            "  Built from source; its license (WTFPL) is in\n"
            "  usr/share/doc/lgogdownloader/COPYING.\n"
            "  Source: https://github.com/Sude-/lgogdownloader\n\n"
            "Adwaita symbolic icons (built into gog-manager)\n"
            "  Copyright GNOME Project; CC-BY-SA-3.0 OR LGPL-3.0-only, used under\n"
            "  LGPL-3.0-only (text in usr/share/licenses/gog-manager/LGPL-3.0-only.txt).\n"
            "  Source: https://gitlab.gnome.org/GNOME/adwaita-icon-theme\n\n"
            f"Packages from the build machine ({pm.name}):\n\n"
        )
        for pkg, version, licenses, source, n in notices:
            out.write(f"{pkg} {version}\n  License: {licenses}\n  Source: {source}\n"
                      f"  Files: {n}\n\n")
    print(f"Shipped the licenses of {len(notices)} packages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
