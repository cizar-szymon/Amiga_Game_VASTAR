#!/usr/bin/env python3
"""Refuse a public release that contains ROMs (tools/make_release.sh).

  python tools/check_no_roms.py ROMDIR PATH...

PATH: a directory (all files below it) or a .zip (all members). Fails (exit 1) when
a file has the same contents as a file in ROMDIR (or in a .zip there), has a ROM-like
name (*.rom, tbp24s10.*, vastar*.zip, pprobe*.zip), or lies in a rom/ directory and
is not ReadMe.txt.
"""
import hashlib
import os
import re
import sys
import zipfile

ROMNAME = re.compile(r"(\.rom$|^tbp24s10\.|^(vastar\d?|pprobe)\.zip$)", re.I)


def digest(data):
    return hashlib.sha1(data).hexdigest()


def rom_digests(romdir):
    out = {}
    for name in os.listdir(romdir):
        path = os.path.join(romdir, name)
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            data = f.read()
        out[digest(data)] = name
        if name.lower().endswith(".zip"):
            with zipfile.ZipFile(path) as z:
                for m in z.namelist():
                    out[digest(z.read(m))] = name + ":" + m
    return out


def members(path):
    """(name, data) of every file under a directory or in a ZIP."""
    if os.path.isdir(path):
        for root, _, files in os.walk(path):
            for f in files:
                p = os.path.join(root, f)
                with open(p, "rb") as fh:
                    yield os.path.relpath(p, path).replace(os.sep, "/"), fh.read()
    else:
        with zipfile.ZipFile(path) as z:
            for m in z.namelist():
                if not m.endswith("/"):
                    yield m, z.read(m)


def main():
    romdir, paths = sys.argv[1], sys.argv[2:]
    roms = rom_digests(romdir)
    bad = []
    for p in paths:
        for name, data in members(p):
            base = name.rsplit("/", 1)[-1]
            parts = name.lower().split("/")
            if digest(data) in roms:
                bad.append(f"{p}: {name} = ROM {roms[digest(data)]}")
            elif ROMNAME.search(base):
                bad.append(f"{p}: {name} has a ROM name")
            elif "rom" in parts[:-1] and base.lower() != "readme.txt":
                bad.append(f"{p}: {name} in a rom/ directory")
    for b in bad:
        print("ROM in a public release:", b, file=sys.stderr)
    if bad:
        sys.exit(1)
    print("no ROMs:", " ".join(paths))


if __name__ == "__main__":
    main()
