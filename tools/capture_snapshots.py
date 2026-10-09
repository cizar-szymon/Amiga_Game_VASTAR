#!/usr/bin/env python3
"""Capture entry/exit snapshots of procedures in VICE for differential tests.

For every hit of a listed procedure the full 64K memory (CPU view: RAM, I/O,
cartridge) and the registers are saved when the procedure is entered and again
when it returns (breakpoint on the return address taken from the stack). The C
port can then start from the entry state, run its version of the procedure and
compare the result with the exit state (port/tests/proc_test.c).

Files in --out, per hit:  <proc>_<n>.in  <proc>_<n>.out  (65536 bytes each)
                          <proc>_<n>.txt (registers: "in A X Y SP" / "out A X Y SP")

Scenarios:
  title  boot the cartridge and record the title screens
  game   additionally start a 1-player game (fakes the fire button)

Usage:
  python tools/capture_snapshots.py --vice <VICE dir> --crt build/wow.crt
      --symbols build/wow.symbols --out build/snapshots --scenario game
      --procs clear_screen,draw_dungeon,big_letters.print_word --count 2
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vice_monitor import Vice, read_symbols  # noqa: E402

BREAK_NO_RE = re.compile(r"BREAK:\s*(\d+)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vice", required=True)
    ap.add_argument("--crt", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--procs", required=True, help="comma separated asm names (scopes with dots)")
    ap.add_argument("--count", type=int, default=2, help="hits to record per procedure")
    ap.add_argument("--skip", type=int, default=0, help="hits to let pass first (to get later game states)")
    ap.add_argument("--prefix", default="", help="added to the snapshot numbers, e.g. 'late' -> move_actor_late3")
    ap.add_argument("--event", metavar="ADDR",
                    help="first run until this address (hex) executes, then rerun the same deterministic "
                         "game and start recording one frame before that moment")
    ap.add_argument("--when-changed", metavar="ADDR:LEN",
                    help="keep only hits where this memory range (hex address) differs between entry and exit")
    ap.add_argument("--scenario", choices=("title", "game"), default="title")
    ap.add_argument("--timeout", type=float, default=120, help="seconds of emulated running to wait")
    args = ap.parse_args()

    sym = read_symbols(args.symbols)
    procs = [p.strip() for p in args.procs.split(",") if p.strip()]
    for p in procs:
        if p not in sym:
            sys.exit(f"unknown procedure {p}")
    os.makedirs(args.out, exist_ok=True)
    tmp_dump = os.path.abspath(os.path.join(args.out, "_dump.bin"))

    def start_scenario(v):
        v.enter()
        if args.scenario == "game":
            v.cmd(f"break {sym['check_if_game_is_started._check']:04x}")
            v.go_until_break()
            v.cmd("delete")
            v.cmd("r x = ff")
            v.cmd(f"r pc = {sym['check_if_game_is_started._fire_pressed']:04x}")

    t_event = None
    if args.event:
        # phase 1: when does the event happen (emulator clock)?
        v1 = Vice(args.vice, os.path.abspath(args.crt), extra=("-warp",))
        try:
            start_scenario(v1)
            v1.cmd(f"break {int(args.event, 16):04x}")
            t_event = v1.go_until_break(timeout=args.timeout)["clock"]
        finally:
            v1.quit()
        print(f"event ${args.event} at clock {t_event}")

    vice = Vice(args.vice, os.path.abspath(args.crt), extra=("-warp",))
    remaining = {p: args.count for p in procs}
    changed_range = None
    if args.when_changed:
        a, l = args.when_changed.split(":")
        changed_range = slice(int(a, 16), int(a, 16) + int(l))
    skipped = {p: 0 for p in procs}
    entry_bp = {}            # proc -> breakpoint number
    stack = []               # [proc, sp, ret, ret_bp, n, entry dump, entry regs]

    def add_break(addr):
        out = vice.cmd(f"break {addr:04x}")
        return int(BREAK_NO_RE.search(out).group(1))

    def dump():
        vice.cmd(f'bsave "{tmp_dump}" 0 0000 ffff')
        with open(tmp_dump, "rb") as f:
            data = f.read()
        if len(data) != 0x10000:
            raise RuntimeError("memory dump failed")
        return data

    def save(name, n, mem_in, regs_in, mem_out, regs_out):
        base = os.path.join(args.out, f"{name}_{n}")
        with open(base + ".in", "wb") as f:
            f.write(mem_in)
        with open(base + ".out", "wb") as f:
            f.write(mem_out)
        with open(base + ".txt", "w") as f:
            f.write("in %02x %02x %02x %02x\n" % (regs_in["a"], regs_in["x"], regs_in["y"], regs_in["sp"]))
            f.write("out %02x %02x %02x %02x\n" % (regs_out["a"], regs_out["x"], regs_out["y"], regs_out["sp"]))
        print(f"  {name}_{n}")

    try:
        start_scenario(vice)
        if t_event:
            # phase 2: run freely (counting frames with an ignored IRQ breakpoint) until
            # about two frames before the event
            bp = add_break(sym["irq_handler"])
            t_now = vice.go_until_break(timeout=args.timeout)["clock"]
            frames = (t_event - t_now) // 17095 - 2
            if frames > 0:
                vice.cmd(f"ignore {bp} {frames:x}")
                vice.go_until_break(timeout=args.timeout)
            vice.cmd(f"delete {bp}")
        for p in procs:
            entry_bp[p] = add_break(sym[p])

        addr_to_proc = {sym[p]: p for p in procs}
        deadline = time.time() + args.timeout
        while (any(remaining.values()) or stack) and time.time() < deadline:
            hit = vice.go_until_break(timeout=args.timeout)
            pc, sp = hit["pc"], hit["sp"]

            # returning: close every open capture with this return address and stack level
            # (several when a procedure falls through into another, e.g. score.print ->
            # score.print.only_player2); captures above them never returned normally
            matching = [i for i, s in enumerate(stack) if s[2] == pc and (s[1] + 2) & 0xFF == sp]
            if matching:
                mem_out = dump()
                for i in range(len(stack) - 1, matching[0] - 1, -1):
                    proc, _, _, ret_bp, n, mem_in, regs_in = stack.pop()
                    vice.cmd(f"delete {ret_bp}")
                    if i in matching:
                        if changed_range and mem_in[changed_range] == mem_out[changed_range]:
                            remaining[proc] += 1        # not interesting, try again
                            if remaining[proc] == 1:
                                entry_bp[proc] = add_break(sym[proc])
                            continue
                        save(proc.replace(".", "_"), n, mem_in, regs_in, mem_out, hit)
                    else:
                        print(f"  dropped {proc}_{n} (left without returning)")
                continue

            proc = addr_to_proc.get(pc)
            if proc is None:
                continue                            # a return address hit too early (other path)
            if stack and stack[-1][0] == proc and stack[-1][1] == sp:
                continue                            # the procedure loops back to its own label
            if not remaining[proc]:
                continue
            if skipped[proc] < args.skip:
                skipped[proc] += 1
                continue
            mem_in = dump()
            ret = (mem_in[0x100 + ((sp + 1) & 0xFF)] | (mem_in[0x100 + ((sp + 2) & 0xFF)] << 8)) + 1
            n = f"{args.prefix}{args.count - remaining[proc]}"
            remaining[proc] -= 1
            if not remaining[proc]:
                vice.cmd(f"delete {entry_bp[proc]}")
            stack.append([proc, sp, ret & 0xFFFF, add_break(ret & 0xFFFF), n, mem_in, hit])
    finally:
        vice.quit()
        if os.path.exists(tmp_dump):
            os.remove(tmp_dump)

    missing = [p for p, r in remaining.items() if r == args.count]
    if missing:
        print("not reached:", ", ".join(missing))


if __name__ == "__main__":
    main()
