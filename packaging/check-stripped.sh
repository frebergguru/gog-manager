#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Hypnotize
#
# check-stripped.sh — refuse a release that carries symbols or debug info.
#
# Every ELF, .exe, .dll and .so that ships must be stripped, including bundled
# third-party libraries. This reads the FINISHED files and fails if any one of
# them still has:
#
#   ELF  a symbol table (.symtab, or any SHT_SYMTAB section), any .debug_* /
#        .zdebug_* section, or .gnu_debugdata (MiniDebugInfo: a packed .symtab)
#   PE   any .debug_* section, or a nonzero COFF symbol count
#
# It parses the headers itself rather than calling readelf/objdump, because
# the host's mingw x86_64 objdump cannot read an ARM64 PE and the release
# covers both. Directories are walked; .apk, .zip and .tar* archives are
# unpacked in memory and every file inside is checked the same way.
#
# Shared with Wolf Run Pinball Arcade. packaging/build-appimage.sh runs it on
# the AppDir and the finished AppImage (its runtime ELF) before release.
#
# Usage:
#   ./packaging/check-stripped.sh AppDir gog-manager-x86_64.AppImage   exit 1 on any failure

set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <file|dir|archive>..." >&2
    exit 2
fi

exec python3 -I - "$@" <<'PY'
import io, os, struct, sys, tarfile, zipfile

def elf_problems(b):
    cls, data = b[4], b[5]
    e = '<' if data == 1 else '>'
    if cls == 2:
        shoff, = struct.unpack_from(e + 'Q', b, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(e + 'HHH', b, 0x3A)
    else:
        shoff, = struct.unpack_from(e + 'I', b, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(e + 'HHH', b, 0x2E)
    if shoff == 0:
        return []
    def sh(i):
        o = shoff + i * shentsize
        name, stype = struct.unpack_from(e + 'II', b, o)
        if cls == 2:
            off, size = struct.unpack_from(e + 'QQ', b, o + 0x18)
            link, = struct.unpack_from(e + 'I', b, o + 0x28)
        else:
            off, size = struct.unpack_from(e + 'II', b, o + 0x10)
            link, = struct.unpack_from(e + 'I', b, o + 0x18)
        return name, stype, off, size, link
    # Extended numbering: the real counts live in section 0.
    if shnum == 0:
        shnum = sh(0)[3]
    if shstrndx == 0xFFFF:
        shstrndx = sh(0)[4]
    _, _, stroff, strsize, _ = sh(shstrndx)
    strtab = b[stroff:stroff + strsize]
    bad = []
    for i in range(shnum):
        n, stype, _, _, _ = sh(i)
        name = strtab[n:strtab.index(b'\0', n)].decode('latin-1')
        SHT_SYMTAB = 2
        if (stype == SHT_SYMTAB or name in ('.symtab', '.gnu_debugdata')
                or name.startswith(('.debug', '.zdebug'))):
            bad.append(name)
    return bad

def pe_problems(b):
    pe, = struct.unpack_from('<I', b, 0x3C)
    if b[pe:pe + 4] != b'PE\0\0':
        return None
    nsec, = struct.unpack_from('<H', b, pe + 6)
    symptr, nsym = struct.unpack_from('<II', b, pe + 12)
    optsize, = struct.unpack_from('<H', b, pe + 20)
    sec = pe + 24 + optsize
    strtab = b[symptr + nsym * 18:] if symptr else b''
    bad = []
    if nsym:
        bad.append('%d COFF symbols' % nsym)
    for i in range(nsec):
        raw = b[sec + i * 40: sec + i * 40 + 8].rstrip(b'\0')
        name = raw.decode('latin-1')
        if name.startswith('/') and name[1:].isdigit() and strtab:
            o = int(name[1:])
            name = strtab[o:strtab.index(b'\0', o)].decode('latin-1')
        if name.startswith(('.debug', '.zdebug')):
            bad.append(name)
    return bad

checked = failed = 0

def check(label, b):
    global checked, failed
    if b[:4] == b'\x7fELF':
        bad = elf_problems(b)
    elif b[:2] == b'MZ' and len(b) > 0x40:
        bad = pe_problems(b)
        if bad is None:
            return
    else:
        return
    checked += 1
    if bad:
        failed += 1
        print('  UNSTRIPPED  %s: %s' % (label, ', '.join(bad)))
    else:
        print('  stripped    %s' % label)

def visit(label, b):
    if b[:4] == b'PK\x03\x04':
        with zipfile.ZipFile(io.BytesIO(b)) as z:
            for n in z.namelist():
                if not n.endswith('/'):
                    visit(label + '!' + n, z.read(n))
        return
    try:
        t = tarfile.open(fileobj=io.BytesIO(b))
    except tarfile.TarError:
        check(label, b)
        return
    with t:
        for m in t.getmembers():
            if m.isfile():
                visit(label + '!' + m.name, t.extractfile(m).read())

for arg in sys.argv[1:]:
    if not os.path.exists(arg):
        print('  MISSING     %s' % arg)
        failed += 1
        continue
    paths = [arg] if os.path.isfile(arg) else sorted(
        os.path.join(d, f) for d, _, fs in os.walk(arg) for f in fs)
    for p in paths:
        if os.path.islink(p):
            continue
        with open(p, 'rb') as f:
            visit(p, f.read())

if failed:
    print('FAIL: %d of %d binaries not stripped — release refused.' % (failed, checked))
    sys.exit(1)
if checked == 0:
    print('FAIL: no binaries found to check.')
    sys.exit(1)
print('OK: all %d binaries stripped.' % checked)
PY
