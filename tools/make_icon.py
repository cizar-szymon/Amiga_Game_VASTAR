#!/usr/bin/env python3
"""Build the Workbench icon of the Amiga port from the game's own sprites.

  python tools/make_icon.py OUT.info [--preview OUT.png] [--tooltypes "SCALE=2" ...]

The icon has a classic 4 colour image (any Workbench) and an OS 3.5+
ColorIcon (FORM ICON with FACE and IMAG chunks, RLE compressed) with the
C64 colours, which AmigaOS 3.2 shows.
"""

import argparse
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SPRITES = os.path.join(HERE, "..", "build", "gfx", "c64_sprites.bin")

C64 = [0x000000, 0xFFFFFF, 0x96282E, 0x5BD6CE, 0x9F2DAD, 0x41B936, 0x2724C4, 0xEFF347,
       0x9F4815, 0x5E3500, 0xDA5F66, 0x474747, 0x787878, 0x91FF84, 0x6864FF, 0xAEAEAE]
BLACK, WHITE, RED, YELLOW, BLUE, LBLUE, LRED, ORANGE = 0, 1, 2, 7, 6, 14, 10, 8

W, H = 64, 50


def sprite(code):
    data = open(SPRITES, "rb").read()
    off = (code - 0x80) * 64
    rows = []
    for y in range(21):
        b = data[off + y * 3:off + y * 3 + 3]
        bits = (b[0] << 16) | (b[1] << 8) | b[2]
        rows.append([(bits >> (23 - x)) & 1 for x in range(24)])
    return rows


def picture():
    img = [[BLACK] * W for _ in range(H)]

    def hline(x0, x1, y, c=BLUE):
        for x in range(x0, x1):
            for t in (0, 1):
                img[y + t][x] = c

    def vline(x, y0, y1, c=BLUE):
        for y in range(y0, y1):
            for t in (0, 1):
                img[y][x + t] = c

    def put(code, x0, y0, c, flip=False):
        for y, row in enumerate(sprite(code)):
            for x, v in enumerate(row):
                if v:
                    xx = x0 + (23 - x if flip else x)
                    if 0 <= xx < W and 0 <= y0 + y < H:
                        img[y0 + y][xx] = c

    # a corner of a dungeon: outer walls with the side door, two inner walls
    hline(2, 62, 1)
    hline(2, 62, 47)
    vline(1, 1, 18)
    vline(1, 30, 49)
    vline(61, 1, 18)
    vline(61, 30, 49)
    img[22][0] = img[23][0] = img[24][0] = RED      # the door arrows
    img[22][63] = img[23][63] = img[24][63] = RED
    hline(40, 60, 33)
    vline(49, 33, 47)

    put(0xA8, 37, 5, YELLOW)                         # the Worluk
    put(0x85, 5, 4, LBLUE)                           # a Burwor
    put(0xBA, 5, 25, YELLOW)                         # the Worrior, firing to the right
    return img


# ------------------------------------------------------------------ icon file

def rle_pack(values, depth):
    """ColorIcon RLE: ILBM byterun1 on a bit stream of `depth` bit values"""
    out_bits = []

    def emit(v, n):
        for i in range(n - 1, -1, -1):
            out_bits.append((v >> i) & 1)

    i = 0
    while i < len(values):
        run = 1
        while i + run < len(values) and values[i + run] == values[i] and run < 128:
            run += 1
        if run >= 3:
            emit(257 - run, 8)
            emit(values[i], depth)
            i += run
            continue
        lit = 0
        while i + lit < len(values) and lit < 128:
            r = 1
            while i + lit + r < len(values) and values[i + lit + r] == values[i + lit] and r < 3:
                r += 1
            if r >= 3:
                break
            lit += 1
        emit(lit - 1, 8)
        for v in values[i:i + lit]:
            emit(v, depth)
        i += lit
    while len(out_bits) % 8:
        out_bits.append(0)
    return bytes(int("".join(map(str, out_bits[k:k + 8])), 2) for k in range(0, len(out_bits), 8))


def chunk(name, data):
    pad = b"\0" if len(data) & 1 else b""
    return name + struct.pack(">I", len(data)) + data + pad


def color_icon(img, colors):
    depth = max(1, (len(colors) - 1).bit_length())
    pixels = rle_pack([c for row in img for c in row], depth)
    palette = b"".join(struct.pack(">BBB", (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255) for rgb in colors)
    face = struct.pack(">BBBBH", W - 1, H - 1, 0, 0x11, len(palette) - 1)
    imag = struct.pack(">BBBBBBHH", 0, len(colors) - 1, 0x02, 1, 0, depth, len(pixels) - 1,
                       len(palette) - 1) + pixels + palette
    body = b"ICON" + chunk(b"FACE", face) + chunk(b"IMAG", imag)
    return b"FORM" + struct.pack(">I", len(body)) + body


# the default Workbench 3.x colours: grey, black, white, blue
WB4 = [0xAAAAAA, 0x000000, 0xFFFFFF, 0x6688BB]


def nearest_wb(rgb):
    r, g, b = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
    best = min(range(4), key=lambda i: (r - (WB4[i] >> 16 & 255)) ** 2 + (g - (WB4[i] >> 8 & 255)) ** 2 +
               (b - (WB4[i] & 255)) ** 2)
    return best


def planar(img):
    """the classic 2 bitplane image; black stays black, the figures white, walls blue"""
    m = {BLACK: 1, BLUE: 3, LBLUE: 3, YELLOW: 2, RED: 2}
    words = (W + 15) // 16
    planes = []
    for p in range(2):
        data = bytearray()
        for row in img:
            bits = 0
            for x in range(words * 16):
                v = m.get(row[x], nearest_wb(C64[row[x]])) if x < W else 0
                bits = (bits << 1) | ((v >> p) & 1)
            data += bits.to_bytes(words * 2, "big")
        planes.append(bytes(data))
    return b"".join(planes)


def tool_types(types):
    out = struct.pack(">I", (len(types) + 1) * 4)
    for t in types:
        b = t.encode("latin-1") + b"\0"
        out += struct.pack(">I", len(b)) + b
    return out


def disk_object(img, types):
    gadget = struct.pack(">IhhhhHHHIIIIIHI", 0, 0, 0, W, H + 1, 0x0004, 0x0003, 0x0001,
                         1, 0, 0, 0, 0, 0, 1)
    head = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIiiIII", 3, 0, 0, 1 if types else 0, -0x80000000, -0x80000000, 0, 0, 16384)
    image = struct.pack(">hhhhhIBBI", 0, 0, W, H, 2, 1, 3, 0, 0) + planar(img)
    colors = sorted(set(c for row in img for c in row))
    remap = {c: i for i, c in enumerate(colors)}
    cimg = [[remap[c] for c in row] for row in img]
    return head + image + (tool_types(types) if types else b"") + color_icon(cimg, [C64[c] for c in colors])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--preview")
    ap.add_argument("--tooltypes", nargs="*", default=[])
    args = ap.parse_args()
    img = picture()
    open(args.out, "wb").write(disk_object(img, args.tooltypes))
    if args.preview:
        from PIL import Image
        p = Image.new("RGB", (W, H))
        p.putdata([((C64[c] >> 16) & 255, (C64[c] >> 8) & 255, C64[c] & 255) for row in img for c in row])
        p.resize((W * 6, H * 6), Image.NEAREST).save(args.preview)
    print(f"{args.out}: {W}x{H}")


if __name__ == "__main__":
    main()
