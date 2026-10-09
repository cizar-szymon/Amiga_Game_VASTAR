#!/usr/bin/env python3
"""Compare the port's VIC-II renderer with VICE, pixel by pixel.

At a few moments (title screen, enemies table, dungeon) the C64 memory is dumped
and a screenshot is taken in VICE. The port renders the same memory dump
(wowport --render) and the two images are compared; the best alignment within
+-8 pixels is searched so a geometry mismatch is reported as an offset.

Usage:
  python tools/compare_render.py --vice <VICE dir> --crt build/wow.crt
      --symbols build/wow.symbols --port build/port/wowport.exe --out build/render_check
"""

import argparse
import os
import subprocess
import sys
import time

from PIL import Image, ImageChops

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vice_monitor import Vice, read_symbols  # noqa: E402

# (name, seconds to run before the snapshot, start a game first)
MOMENTS = [("title", 2.0, False), ("enemies", 8.5, False), ("dungeon", 9.0, True)]


def compare(vice_png, port_bmp, diff_png):
    a = Image.open(vice_png).convert("RGB")
    b = Image.open(port_bmp).convert("RGB")
    best = None
    for dy in range(-8, 9):
        for dx in range(-8, 9):
            # overlap of a and b shifted by (dx, dy)
            x0, y0 = max(0, dx), max(0, dy)
            x1, y1 = min(a.width, b.width + dx), min(a.height, b.height + dy)
            if x1 - x0 < 300 or y1 - y0 < 180:
                continue
            ca = a.crop((x0, y0, x1, y1))
            cb = b.crop((x0 - dx, y0 - dy, x1 - dx, y1 - dy))
            diff = ImageChops.difference(ca, cb).convert("L").point(lambda v: 255 if v > 24 else 0)
            bad = sum(1 for v in diff.tobytes() if v)
            if best is None or bad < best[0]:
                best = (bad, dx, dy, diff, (x1 - x0) * (y1 - y0))
    bad, dx, dy, diff, total = best
    diff.save(diff_png)
    return bad, total, dx, dy, a.size, b.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vice", required=True)
    ap.add_argument("--crt", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    sym = read_symbols(args.symbols)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    ok = True
    for name, seconds, game in MOMENTS:
        # same palette as tools/export_gfx.py (VICE otherwise generates its own colors)
        vice = Vice(args.vice, os.path.abspath(args.crt),
                    extra=("-VICIIextpal", "-VICIIpalette", "colodore.vpl"))
        try:
            vice.enter()
            if game:
                vice.cmd(f"break {sym['check_if_game_is_started._check']:04x}")
                vice.go_until_break()
                vice.cmd("delete")
                vice.cmd("r x = ff")
                vice.cmd(f"r pc = {sym['check_if_game_is_started._fire_pressed']:04x}")
            vice.resume()
            time.sleep(seconds)
            vice.enter()
            dump = os.path.join(out, f"{name}.bin")
            shot = os.path.join(out, f"{name}_vice.png")
            vice.cmd(f'bsave "{dump}" 0 0000 ffff')
            vice.cmd(f'screenshot "{shot}" 2')
        finally:
            vice.quit()
        port_bmp = os.path.join(out, f"{name}_port.bmp")
        subprocess.run([os.path.abspath(args.port), "--render", dump, port_bmp], check=True)
        bad, total, dx, dy, vs, ps = compare(shot, port_bmp, os.path.join(out, f"{name}_diff.png"))
        print(f"{name:8} VICE {vs[0]}x{vs[1]}, port {ps[0]}x{ps[1]}, offset ({dx},{dy}): "
              f"{bad} of {total} pixels differ")
        ok &= bad == 0
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
