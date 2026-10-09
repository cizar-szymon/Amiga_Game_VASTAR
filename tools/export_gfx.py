#!/usr/bin/env python3
"""Export Wizard of Wor graphics (charset + sprites) for the Amiga port.

Rebuilds the character set ($0800-$0FFF) and sprite area ($2000-$36BF) exactly
as the game does at startup (init_cset / create_sprites), using the assembled
cartridge image and its symbol file (64tass --dump-labels), then writes:

  c64_charset.bin    2048 bytes, C64 RAM image $0800-$0FFF
  c64_sprites.bin    87 * 64 bytes, C64 RAM image of sprites $80-$D6
  gfx_data.h/.c      C tables for the port (palette, glyphs, sprite frames)
  *.png              previews

The C64 images can be checked against a RAM dump from VICE with
tools/verify_gfx_vice.py.

Usage:
  python tools/export_gfx.py --bin build/wow.bin --symbols build/wow.symbols
                             --chargen <VICE>/C64/chargen-901225-01.bin
                             --palette <VICE>/C64/colodore.vpl --out build/gfx
"""

import argparse
import os
import re
import sys

CART_BASE = 0x8000

# sprite numbers as seen by the VIC (sprite pointer value), $2000 = sprite $80
FIRST_SPRITE = 0x80
NO_SPRITE = 0xD6           # empty sprite used to hide actors
NUM_SPRITES = NO_SPRITE - FIRST_SPRITE + 1

REQUIRED_LABELS = ("cset_rom_custom1", "cset_rom_custom2", "cset_rom_custom_packed", "sprites_rom",
                   "create_sprites.sprites_to_mirror_horizontally")


def read_labels(path):
    """Read numeric symbols from a 64tass --dump-labels file ('file:line:col: name = $value')."""
    labels = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r": ([A-Za-z_][\w.]*) = \$([0-9a-fA-F]+)\s*$", line)
            if m:
                labels[m.group(1)] = int(m.group(2), 16)
    return labels


class C64Ram:
    """64 KB of RAM with the 16K cartridge visible at $8000-$BFFF for reads."""

    def __init__(self, cart):
        self.ram = bytearray(0x10000)
        self.cart = cart

    def read(self, addr):
        addr &= 0xFFFF
        if CART_BASE <= addr < CART_BASE + len(self.cart):
            return self.cart[addr - CART_BASE]
        return self.ram[addr]

    def write(self, addr, value):
        self.ram[addr & 0xFFFF] = value & 0xFF


def init_cset(mem, lbl, chargen):
    """Port of init_cset: builds the charset at $0800."""
    for x in range(0x400):                 # _clr_cset: $0800-$0BFF
        mem.write(0x0800 + x, 0)

    # copy $300 bytes from cset_rom_custom1 to $09E8 (Y loop runs 0,255..1 per page)
    src, dst = lbl["cset_rom_custom1"], 0x09E8
    for page in range(3):
        for y in range(256):
            mem.write(dst + page * 256 + y, mem.read(src + page * 256 + y))

    # copy $70 bytes cset_rom_custom2 -> $0808
    src = lbl["cset_rom_custom2"]
    for y in range(0x70):
        mem.write(0x0808 + y, mem.read(src + y))

    # copy from char rom: CHAR_ROM_A-1+Y -> $0B48+Y, CHAR_ROM_B-1+Y -> $0C18+Y, Y=$D0..1
    for y in range(1, 0xD1):
        mem.write(0x0B48 + y, chargen[0x0008 - 1 + y])
        mem.write(0x0C18 + y, chargen[0x0180 - 1 + y])

    # copy packed chars: 5 of 8 lines stored
    src = lbl["cset_rom_custom_packed"]
    dst = 0x0880 + 3                       # first $14 chars: upper 3 lines empty
    for _ in range(0x14):
        for y in range(5):
            mem.write(dst + y, mem.read(src + y))
        dst += 8
        src += 5
    dst -= 3                               # next $19 chars: lower 3 lines empty
    for _ in range(0x19):
        for y in range(5):
            mem.write(dst + y, mem.read(src + y))
        dst += 8
        src += 5


def create_sprites(mem, lbl):
    """Port of create_sprites: builds 86 sprites at $2000."""
    for x in range(0x5B * 0x40):           # clear 91 sprites
        mem.write(0x2000 + x, 0)

    # copy $28 sprites, 54 bytes each (last 3 lines are not stored)
    src, dst = lbl["sprites_rom"], 0x2000
    for _ in range(0x28):
        for y in range(0x36):
            mem.write(dst + y, mem.read(src + y))
        src += 0x36
        dst += 0x40

    # mirror vertically $12 sprites, starting with sprite $96
    for sprite_no in range(0x96, 0x96 + 0x12):
        src = sprite_no * 64 + 0x33
        for _ in range(18):
            for y in range(3):
                mem.write(dst + y, mem.read(src + y))
            dst += 3
            src -= 3
        dst += 10

    # mirror horizontally $1C sprites (table is walked backwards)
    table = lbl["create_sprites.sprites_to_mirror_horizontally"]
    for x in range(0x1B, -1, -1):
        src = mem.read(table + x) * 64
        for y in range(0x33, -1, -3):
            left, right = mem.read(src + y), mem.read(src + y + 2)
            mem.write(dst + y, reverse_bits(right))
            mem.write(dst + y + 2, reverse_bits(left))
            mem.write(dst + y + 1, reverse_bits(mem.read(src + y + 1)))
        dst += 0x40


def reverse_bits(b):
    return int(f"{b:08b}"[::-1], 2)


def read_vpl(path):
    """VICE palette file: lines 'RR GG BB [dither]'."""
    colors = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.split("#")[0].strip()
            parts = line.split()
            if len(parts) >= 3:
                colors.append(tuple(int(p, 16) for p in parts[:3]))
    if len(colors) < 16:
        sys.exit(f"palette {path}: expected 16 colors, got {len(colors)}")
    return colors[:16]


def rgb_to_ocs(rgb):
    """8-bit RGB -> Amiga OCS $0RGB (4 bits per channel, rounded)."""
    r, g, b = ((c * 15 + 127) // 255 for c in rgb)
    return (r << 8) | (g << 4) | b


def mc_masks(glyph):
    """Split a multicolor glyph into 1bpp masks with doubled pixels: (MC1/$D022, MC2/$D023, FG/color RAM)."""
    masks = ([], [], [])
    for row in glyph:
        m = [0, 0, 0]
        for pair in range(4):
            v = (row >> (6 - pair * 2)) & 3
            if v:
                m[v - 1] |= 0b11 << (6 - pair * 2)
        for i in range(3):
            masks[i].append(m[i])
    return masks


# ----------------------------------------------------------------- output

def write_c(out_dir, palette, charset, sprites):
    glyphs = [charset[i * 8:(i + 1) * 8] for i in range(256)]
    frames = []
    for n in range(NUM_SPRITES):
        data = sprites[n * 64:n * 64 + 63]
        frames.append([(data[r * 3] << 24) | (data[r * 3 + 1] << 16) | (data[r * 3 + 2] << 8) for r in range(21)])

    h = f"""/* Generated by tools/export_gfx.py - do not edit */
#ifndef GFX_DATA_H
#define GFX_DATA_H

#include <stdint.h>

/* C64 colors 0-15 as Amiga OCS $0RGB values and as 0xRRGGBB (VICE palette) */
extern const uint16_t c64_palette_ocs[16];
extern const uint32_t c64_palette_rgb[16];

/* 256 hires glyphs, 8 bytes each (bit 7 = leftmost pixel) - C64 RAM $0800 */
extern const uint8_t charset_hires[256][8];

/* the same glyphs interpreted as multicolor: masks for pixels taking their color from
   [0] $D022 (MC1), [1] $D023 (MC2), [2] color RAM; pixels in no mask use $D021 */
extern const uint8_t charset_mc[256][3][8];

/* sprite frames indexed by (C64 sprite pointer - SPRITE_FIRST), 24x21 hires,
   one uint32_t per line, bit 31 = leftmost pixel, bits 7-0 always zero */
#define SPRITE_FIRST 0x{FIRST_SPRITE:02X}
#define SPRITE_NONE  0x{NO_SPRITE:02X}
#define SPRITE_COUNT {NUM_SPRITES}
#define SPRITE_LINES 21
extern const uint32_t sprite_frames[SPRITE_COUNT][SPRITE_LINES];

/* the same data in C64 RAM layout, for the PC port that emulates the VIC-II:
   charset at $0800-$0FFF, sprite blocks at $2000 (64 bytes each, from SPRITE_FIRST) */
#define C64_CHARSET_ADDR 0x0800
#define C64_SPRITES_ADDR 0x2000
extern const uint8_t c64_charset_ram[2048];
extern const uint8_t c64_sprites_ram[SPRITE_COUNT * 64];

#endif
"""
    lines = ["/* Generated by tools/export_gfx.py - do not edit */", '#include "gfx_data.h"', ""]
    lines.append("const uint16_t c64_palette_ocs[16] = {")
    lines.append("    " + ", ".join(f"0x{rgb_to_ocs(c):04X}" for c in palette))
    lines.append("};\n")
    lines.append("const uint32_t c64_palette_rgb[16] = {")
    lines.append("    " + ", ".join(f"0x{r:02X}{g:02X}{b:02X}" for r, g, b in palette))
    lines.append("};\n")

    lines.append("const uint8_t charset_hires[256][8] = {")
    for i, g in enumerate(glyphs):
        lines.append("    {" + ",".join(f"0x{b:02X}" for b in g) + f"}}, /* ${i:02X} */")
    lines.append("};\n")

    lines.append("const uint8_t charset_mc[256][3][8] = {")
    for i, g in enumerate(glyphs):
        parts = ["{" + ",".join(f"0x{b:02X}" for b in m) + "}" for m in mc_masks(g)]
        lines.append("    {" + ", ".join(parts) + f"}}, /* ${i:02X} */")
    lines.append("};\n")

    lines.append("const uint32_t sprite_frames[SPRITE_COUNT][SPRITE_LINES] = {")
    for n, f in enumerate(frames):
        lines.append(f"    {{ /* ${FIRST_SPRITE + n:02X} */")
        for r in range(0, 21, 7):
            lines.append("        " + ",".join(f"0x{v:08X}" for v in f[r:r + 7]) + ",")
        lines.append("    },")
    lines.append("};")

    for name, data in (("c64_charset_ram[2048]", charset), ("c64_sprites_ram[SPRITE_COUNT * 64]", sprites)):
        lines.append("")
        lines.append(f"const uint8_t {name} = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ",".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
        lines.append("};")

    with open(os.path.join(out_dir, "gfx_data.h"), "w", newline="\n") as fh:
        fh.write(h)
    with open(os.path.join(out_dir, "gfx_data.c"), "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def write_previews(out_dir, palette, charset, sprites):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow not installed - skipping PNG previews")
        return

    scale = 3
    # charset: 16x16 grid, hires (white) and multicolor (MC1=red, MC2=blue, FG=yellow on black)
    for name, mc in (("charset_hires.png", False), ("charset_mc.png", True)):
        cell = 10
        img = Image.new("RGB", (16 * cell, 16 * cell), (40, 40, 40))
        px = img.load()
        for c in range(256):
            ox, oy = (c % 16) * cell + 1, (c // 16) * cell + 1
            for y in range(8):
                row = charset[c * 8 + y]
                for x in range(8):
                    if mc:
                        v = (row >> (6 - (x & 6))) & 3
                        col = [palette[0], palette[2], palette[6], palette[7]][v]
                    else:
                        col = palette[1] if row & (0x80 >> x) else palette[0]
                    px[ox + x, oy + y] = col
        img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(os.path.join(out_dir, name))

    # sprites: 8 per row, labelled with their sprite pointer value
    cols, cw, ch = 8, 28, 34
    rows = (NUM_SPRITES + cols - 1) // cols
    img = Image.new("RGB", (cols * cw * scale, rows * ch * scale), (40, 40, 40))
    draw = ImageDraw.Draw(img)
    for n in range(NUM_SPRITES):
        ox, oy = (n % cols) * cw, (n // cols) * ch
        for y in range(21):
            for x in range(24):
                byte = sprites[n * 64 + y * 3 + x // 8]
                col = palette[1] if byte & (0x80 >> (x % 8)) else palette[0]
                draw.rectangle([(ox + 2 + x) * scale, (oy + 11 + y) * scale,
                                (ox + 3 + x) * scale - 1, (oy + 12 + y) * scale - 1], fill=col)
        draw.text(((ox + 2) * scale, (oy + 1) * scale), f"${FIRST_SPRITE + n:02X}", fill=(255, 255, 0))
    img.save(os.path.join(out_dir, "sprites.png"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="assembled cartridge image (wow.bin)")
    ap.add_argument("--symbols", required=True, help="64tass --dump-labels output")
    ap.add_argument("--chargen", required=True, help="C64 character ROM (4096 bytes)")
    ap.add_argument("--palette", required=True, help="VICE .vpl palette")
    ap.add_argument("--out", required=True, help="output directory")
    args = ap.parse_args()

    cart = open(args.bin, "rb").read()
    if len(cart) != 0x4000:
        sys.exit(f"{args.bin}: expected 16384 bytes, got {len(cart)}")
    chargen = open(args.chargen, "rb").read()
    if len(chargen) != 0x1000:
        sys.exit(f"{args.chargen}: expected 4096 bytes, got {len(chargen)}")
    lbl = read_labels(args.symbols)
    for name in REQUIRED_LABELS:
        if name not in lbl:
            sys.exit(f"label {name} not found in {args.symbols}")
    palette = read_vpl(args.palette)

    mem = C64Ram(cart)
    init_cset(mem, lbl, chargen)
    create_sprites(mem, lbl)

    charset = bytes(mem.ram[0x0800:0x1000])
    sprites = bytes(mem.ram[0x2000:0x2000 + NUM_SPRITES * 64])

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "c64_charset.bin"), "wb") as f:
        f.write(charset)
    with open(os.path.join(args.out, "c64_sprites.bin"), "wb") as f:
        f.write(sprites)
    write_c(args.out, palette, charset, sprites)
    write_previews(args.out, palette, charset, sprites)
    print(f"exported charset (256 chars) and {NUM_SPRITES} sprite frames to {args.out}")


if __name__ == "__main__":
    main()
