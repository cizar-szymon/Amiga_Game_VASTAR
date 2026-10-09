#!/usr/bin/env python3
"""Measure the cost of normal_gameplay_loop iterations in VICE.

The game's main loop is not synchronized to the screen: monsters move every
(speed+1)-th time the loop processes them, so their real speed depends on how
many CPU cycles a loop iteration takes. This tool starts a 1-player game in
VICE (NTSC), samples the emulator clock at every pass of normal_gameplay_loop
in several bursts spread over the game, and classifies each iteration by the
state of the actor it processed.

Output (in --out):
  loop_samples.csv   one row per iteration
  loop_timing.md     summary: cycles per iteration type, iterations per frame,
                     monster moves per second

Usage:
  python tools/measure_loop_timing.py --vice <VICE dir> --crt build/wow.crt
                                      --symbols build/wow.symbols --out build/timing
"""

import argparse
import collections
import csv
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vice_monitor import Vice, read_symbols  # noqa: E402

NTSC_CYCLES_PER_FRAME = 65 * 263
NTSC_FPS = 1022727 / NTSC_CYCLES_PER_FRAME
MAX_ACTORS = 8


def classify(actor, atype, timer):
    if atype & 0x80:
        return "dead"
    if atype == 0:
        return "dying"
    if actor >= 2:
        return "monster_move" if (timer - 1) & 0xFF == 0 else "monster_wait"
    return "player_move" if timer & 0x80 else "player_wait"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vice", required=True)
    ap.add_argument("--crt", required=True)
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--bursts", type=int, default=10)
    ap.add_argument("--samples", type=int, default=200, help="loop iterations per burst")
    ap.add_argument("--gap", type=float, default=2.0, help="seconds of free running between bursts")
    args = ap.parse_args()

    sym = read_symbols(args.symbols)
    loop = sym["normal_gameplay_loop"]
    wait_fire = sym["check_if_game_is_started._check"]
    fire_pressed = sym["check_if_game_is_started._fire_pressed"]
    zp_lo, zp_hi = sym["actual_actor"], sym["animation_timer_tbl"] + MAX_ACTORS - 1
    types_lo, types_hi = sym["actor_speed_tbl"], sym["actor_type_tbl"] + MAX_ACTORS - 1

    vice = Vice(args.vice, os.path.abspath(args.crt))
    rows = []
    try:
        # start a 1-player game: wait in the title screen fire check, fake "fire on port 1"
        vice.enter()
        vice.cmd(f"break {wait_fire:04x}")
        vice.go_until_break()
        vice.cmd("delete")
        vice.cmd("r x = ff")
        vice.cmd(f"r pc = {fire_pressed:04x}")

        for burst in range(args.bursts):
            vice.resume()
            time.sleep(args.gap)
            vice.enter()
            vice.cmd(f"break {loop:04x}")
            prev = None
            for _ in range(args.samples + 1):
                hit = vice.go_until_break()
                zp = vice.mem(zp_lo, zp_hi)
                page2 = vice.mem(types_lo, types_hi)
                state = {
                    "clock": hit["clock"], "line": hit["line"],
                    "actual_actor": zp[0],
                    "timers": zp[sym["animation_timer_tbl"] - zp_lo:],
                    "types": page2[sym["actor_type_tbl"] - types_lo:],
                    "speeds": page2[:MAX_ACTORS],
                }
                if prev is not None:
                    actor = (prev["actual_actor"] + 1) & 7
                    kind = classify(actor, prev["types"][actor], prev["timers"][actor])
                    alive_monsters = sum(1 for a in range(2, 8) if 0 < prev["types"][a] < 0x80)
                    rows.append({
                        "burst": burst, "clock": prev["clock"], "cycles": state["clock"] - prev["clock"],
                        "actor": actor, "kind": kind, "radar": int(actor == 7),
                        "type": prev["types"][actor], "speed": prev["speeds"][actor],
                        "alive_monsters": alive_monsters,
                    })
                prev = state
            vice.cmd("delete")
            print(f"burst {burst + 1}/{args.bursts}: {len(rows)} iterations")
    finally:
        vice.quit()

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "loop_samples.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    write_summary(args.out, rows)


def write_summary(out, rows):
    # iterations that leave the loop (dying -> monster_is_dead -> worluk, game over, ...) are outliers
    normal = [r for r in rows if r["cycles"] < 4 * NTSC_CYCLES_PER_FRAME]

    lines = ["# normal_gameplay_loop timing (VICE, NTSC)", "",
             f"Iterations measured: {len(rows)} ({len(rows) - len(normal)} outliers > 4 frames left out).",
             f"Cycles per frame (NTSC): {NTSC_CYCLES_PER_FRAME}. Cycle counts include IRQ and VIC bad lines.", "",
             "## Cycles per iteration by type", "",
             "| Type | Radar | Count | Mean | Median | Min | Max |", "|---|---|---|---|---|---|---|"]
    groups = collections.defaultdict(list)
    for r in normal:
        groups[(r["kind"], r["radar"])].append(r["cycles"])
    for (kind, radar), c in sorted(groups.items()):
        lines.append(f"| {kind} | {'yes' if radar else ''} | {len(c)} | {statistics.mean(c):.0f} | "
                     f"{statistics.median(c):.0f} | {min(c)} | {max(c)} |")

    lines += ["", "## Per burst", "",
              "| Burst | Alive monsters | Iterations/frame | Full actor rounds/s | Monster moves/s (per monster) |",
              "|---|---|---|---|---|"]
    for burst in sorted({r["burst"] for r in normal}):
        b = [r for r in normal if r["burst"] == burst]
        cycles = sum(r["cycles"] for r in b)
        frames = cycles / NTSC_CYCLES_PER_FRAME
        seconds = frames / NTSC_FPS
        monsters = collections.Counter(r["actor"] for r in b if r["kind"] == "monster_move")
        alive = statistics.mode(r["alive_monsters"] for r in b)
        per_monster = ", ".join(f"#{a}: {n / seconds:.1f}" for a, n in sorted(monsters.items()))
        lines.append(f"| {burst} | {alive} | {len(b) / frames:.1f} | {len(b) / 8 / seconds:.1f} | {per_monster} |")

    with open(os.path.join(out, "loop_timing.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
