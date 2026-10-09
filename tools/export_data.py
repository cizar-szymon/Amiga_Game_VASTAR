#!/usr/bin/env python3
"""Export the game's data and symbols for the Amiga port.

Instead of hand-picking tables, the whole 16K cartridge image is exported as a
C array together with a header that names every symbol of the disassembly:

  wow_rom.h / wow_rom.c   const uint8_t wow_rom[16384] + ROM8()/ROM16() accessors
  wow_symbols.h           #defines for
                            - constants            (e.g. THORWOR, SFX_PLAYER_SHOOTS)
                            - RAM variables        (zero page, $0200 page, screen, ...)
                            - I/O registers        (VIC, SID, CIA, color RAM)
                            - ROM data tables      (with size and source line)
                            - ROM code entry points (procedures, for reference/debugging)
  data_map.md             the ROM data tables as a readable list

Addresses get the prefix A_ (so they cannot clash with C identifiers such as a
function called clear_screen) and scoped names are joined with a double
underscore: draw_dungeon.block_offsets -> A_draw_dungeon__block_offsets.
Constants keep their names (THORWOR). Local code labels (_name) are left out.

Usage:
  python tools/export_data.py --bin build/wow.bin --symbols build/wow.symbols
                              --listing build/wow.lst --out build/data
"""

import argparse
import os
import re
import sys

ROM_BASE = 0x8000
ROM_SIZE = 0x4000

SYMBOL_RE = re.compile(r"^(?P<file>[^:]+):(?P<line>\d+):\d+: (?P<name>[A-Za-z_][\w.]*) = (?P<value>.+)$")
LISTING_RE = re.compile(r"^(?P<kind>[.>])(?P<addr>[0-9a-f]{4})\t(?P<bytes>(?:[0-9a-f]{2} ?)*)", re.I)


def parse_value(text):
    text = text.strip()
    m = re.fullmatch(r"\$([0-9a-fA-F]+)", text)
    if m:
        return int(m.group(1), 16)
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    return None                     # lists, strings, functions, '?' ...


def read_symbols(path):
    symbols = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = SYMBOL_RE.match(line.rstrip("\n"))
            if not m:
                continue
            value = parse_value(m.group("value"))
            if value is None:
                continue
            symbols.append({"name": m.group("name"), "value": value,
                            "file": os.path.basename(m.group("file")), "line": int(m.group("line"))})
    return symbols


def read_listing(path):
    """Return {address: 'code' | 'data'} for every byte emitted into the ROM."""
    kinds = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LISTING_RE.match(line)
            if not m:
                continue
            addr = int(m.group("addr"), 16)
            nbytes = len(m.group("bytes").split())
            kind = "code" if m.group("kind") == "." else "data"
            for i in range(nbytes):
                if ROM_BASE <= addr + i < ROM_BASE + ROM_SIZE:
                    kinds.setdefault(addr + i, kind)
    return kinds


def c_name(name, group):
    name = name.replace(".", "__")
    return name if group == "const" else "A_" + name


def is_address_symbol(sym, source_lines):
    """Labels are addresses. An assignment 'NAME = expr' is a plain constant when its value fits
    in a byte and expr refers neither to '*' nor to a variable (aliases like
    'screen_ptr = tmp_0014_ptr' are addresses)."""
    src = source_lines.get((sym["file"], sym["line"]), "").split(";")[0]
    last = sym["name"].split(".")[-1]
    m = re.match(r"^\s*(\w+)\s*:?=\s*(.*)$", src)
    if not m or m.group(1) != last:
        return True                                   # a label
    expr = re.sub(r"\$[0-9a-fA-F]+", "", m.group(2))
    refers_to_address = "*" in expr or re.search(r"\b[a-z_][a-z0-9_]*\b", expr)
    return bool(refers_to_address) or not 0 <= sym["value"] <= 0xFF


def classify(sym, kinds):
    v = sym["value"]
    if ROM_BASE <= v < ROM_BASE + ROM_SIZE:
        return kinds.get(v, "data")
    if 0xD000 <= v < 0xE000:
        return "io"
    if v >= 0xC000:
        return "ext"                # Magic Voice cartridge ROM, KERNAL
    return "ram"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--symbols", required=True, help="64tass --dump-labels output")
    ap.add_argument("--listing", required=True, help="64tass -L listing")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rom = open(args.bin, "rb").read()
    if len(rom) != ROM_SIZE:
        sys.exit(f"{args.bin}: expected {ROM_SIZE} bytes, got {len(rom)}")
    symbols = read_symbols(args.symbols)
    kinds = read_listing(args.listing)

    src_dir = os.path.dirname(os.path.abspath(args.symbols))
    source_lines = {}
    for fname in {s["file"] for s in symbols}:
        for cand in (os.path.join(src_dir, "..", fname), fname):
            if os.path.exists(cand):
                with open(cand, encoding="utf-8", errors="replace") as f:
                    for i, line in enumerate(f, 1):
                        source_lines[(fname, i)] = line.rstrip("\n")
                break

    groups = {"const": [], "ram": [], "io": [], "ext": [], "data": [], "code": []}
    seen = {}
    for sym in symbols:
        last = sym["name"].split(".")[-1]
        if last == "_" or re.fullmatch(r"_\d+", last):
            continue                                    # macro temporaries
        if not is_address_symbol(sym, source_lines):
            group = "const"
        else:
            group = classify(sym, kinds)
            if group == "code" and last.startswith("_"):
                continue                                # local branch targets
        cname = c_name(sym["name"], group)
        if cname in seen and seen[cname] != sym["value"]:
            sys.exit(f"symbol name clash: {cname}")
        if cname in seen:
            continue
        seen[cname] = sym["value"]
        groups[group].append(dict(sym, cname=cname))

    # sizes of ROM data tables: up to the next ROM symbol or the end of the data run
    rom_addrs = sorted({s["value"] for g in ("data", "code") for s in groups[g]} | {ROM_BASE + ROM_SIZE})
    for s in groups["data"]:
        nxt = next(a for a in rom_addrs if a > s["value"])
        end = s["value"]
        while end < nxt and kinds.get(end, "data") == "data":
            end += 1
        s["size"] = end - s["value"]

    for g in groups.values():
        g.sort(key=lambda s: (s["value"], s["cname"]))

    os.makedirs(args.out, exist_ok=True)
    write_rom(args.out, rom)
    write_symbols(args.out, groups)
    write_data_map(args.out, groups["data"], rom)
    counts = ", ".join(f"{len(v)} {k}" for k, v in groups.items())
    print(f"exported ROM image and symbols ({counts}) to {args.out}")


def write_rom(out, rom):
    h = f"""/* Generated by tools/export_data.py - do not edit */
#ifndef WOW_ROM_H
#define WOW_ROM_H

#include <stdint.h>

#define WOW_ROM_BASE 0x{ROM_BASE:04X}
#define WOW_ROM_SIZE 0x{ROM_SIZE:04X}

/* the 16K cartridge image as the C64 sees it at $8000-$BFFF */
extern const uint8_t wow_rom[WOW_ROM_SIZE];

/* read a byte / little-endian word at a C64 cartridge address (see wow_symbols.h) */
#define ROM8(addr)  (wow_rom[(addr) - WOW_ROM_BASE])
#define ROM16(addr) ((uint16_t)(ROM8(addr) | (ROM8((addr) + 1) << 8)))

#endif
"""
    lines = ["/* Generated by tools/export_data.py - do not edit */", '#include "wow_rom.h"', "",
             "const uint8_t wow_rom[WOW_ROM_SIZE] = {"]
    for i in range(0, ROM_SIZE, 16):
        lines.append("    " + ",".join(f"0x{b:02X}" for b in rom[i:i + 16]) + f", /* ${ROM_BASE + i:04X} */")
    lines.append("};")
    with open(os.path.join(out, "wow_rom.h"), "w", newline="\n") as f:
        f.write(h)
    with open(os.path.join(out, "wow_rom.c"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_symbols(out, groups):
    titles = {
        "const": "constants",
        "ram": "RAM variables and locations (zero page, $0200 page, screen RAM, charset, sprites)",
        "io": "I/O: VIC-II, SID, CIA registers and color RAM",
        "ext": "external ROM entry points (Magic Voice cartridge, KERNAL)",
        "data": "ROM data tables (size = bytes up to the next symbol)",
        "code": "ROM code entry points",
    }
    lines = ["/* Generated by tools/export_data.py - do not edit */",
             "#ifndef WOW_SYMBOLS_H", "#define WOW_SYMBOLS_H", ""]
    for g in ("const", "ram", "io", "ext", "data", "code"):
        lines.append(f"/* ---- {titles[g]} ---- */")
        for s in groups[g]:
            v = s["value"]
            val = f"0x{v:04X}" if g != "const" or v > 9 else str(v)
            if g == "const" and v < 0:
                val = f"({v})"
            note = f"{s['file']}:{s['line']}"
            if g == "data":
                note += f", {s['size']} bytes"
                lines.append(f"#define {s['cname']:<48} {val:<8} /* {note} */")
                lines.append(f"#define {s['cname'] + '_SIZE':<48} {s['size']}")
            else:
                lines.append(f"#define {s['cname']:<48} {val:<8} /* {note} */")
        lines.append("")
    lines.append("#endif")
    with open(os.path.join(out, "wow_symbols.h"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_data_map(out, data, rom):
    lines = ["# ROM data tables", "",
             "Generated by `tools/export_data.py`. Size = bytes up to the next symbol.", "",
             "| Address | Size | Symbol | Source | First bytes |", "|---|---|---|---|---|"]
    for s in data:
        off = s["value"] - ROM_BASE
        preview = " ".join(f"{b:02X}" for b in rom[off:off + min(s["size"], 8)])
        if s["size"] > 8:
            preview += " …"
        lines.append(f"| ${s['value']:04X} | {s['size']} | `{s['name']}` | {s['file']}:{s['line']} | {preview} |")
    with open(os.path.join(out, "data_map.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
