#!/usr/bin/env python3
"""Check the exported C64 charset/sprites against real RAM from VICE.

Starts x64sc with the cartridge and the remote monitor enabled, lets the game
initialize, dumps $0800-$0FFF and the sprite area through the monitor and
compares them with build/gfx/c64_charset.bin and c64_sprites.bin.

Usage:
  python tools/verify_gfx_vice.py --vice <VICE dir> --crt build/wow.crt --gfx build/gfx
"""

import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

PORT = 6510
CSET_INITIALIZED_LEN = 0x0CE8 - 0x0800


def monitor_command(sock, cmd):
    sock.sendall((cmd + "\n").encode("ascii"))
    time.sleep(0.5)
    sock.settimeout(0.5)
    out = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    return out.decode("ascii", "replace")


def compare(name, expected_path, actual_path, length=None):
    expected = open(expected_path, "rb").read()[:length]
    actual = open(actual_path, "rb").read()
    if len(actual) < len(expected):
        print(f"{name}: dump too short ({len(actual)} < {len(expected)})")
        return False
    diffs = [i for i in range(len(expected)) if expected[i] != actual[i]]
    if diffs:
        print(f"{name}: {len(diffs)} differing bytes, first at offset ${diffs[0]:04X}")
        return False
    print(f"{name}: OK ({len(expected)} bytes identical)")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vice", required=True, help="VICE directory (with x64sc.exe)")
    ap.add_argument("--crt", required=True)
    ap.add_argument("--gfx", required=True, help="output dir of export_gfx.py")
    ap.add_argument("--wait", type=float, default=3.0, help="seconds to let the game initialize")
    args = ap.parse_args()

    sprites_len = os.path.getsize(os.path.join(args.gfx, "c64_sprites.bin"))
    tmp = tempfile.mkdtemp(prefix="wow_verify_")
    cset_dump = os.path.join(tmp, "cset.bin")
    spr_dump = os.path.join(tmp, "sprites.bin")

    exe = os.path.join(args.vice, "x64sc.exe")
    proc = subprocess.Popen([exe, "-default", "-ntsc", "-warp", "-sounddev", "dummy",
                             "-remotemonitor", "-remotemonitoraddress", f"ip4://127.0.0.1:{PORT}",
                             "-cartcrt", os.path.abspath(args.crt)], cwd=args.vice)
    try:
        time.sleep(args.wait)
        sock = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        monitor_command(sock, f'bsave "{cset_dump}" 0 0800 0fff')
        monitor_command(sock, f'bsave "{spr_dump}" 0 2000 {0x2000 + sprites_len - 1:04x}')
        monitor_command(sock, "quit")
        sock.close()
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    # init_cset leaves $0CE8-$0FFF (chars $9D-$FF) uninitialized - they hold whatever was in RAM at
    # power-on and the game never puts them on screen, so only the initialized part is compared
    ok = compare("charset", os.path.join(args.gfx, "c64_charset.bin"), cset_dump, CSET_INITIALIZED_LEN)
    ok &= compare("sprites", os.path.join(args.gfx, "c64_sprites.bin"), spr_dump)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
