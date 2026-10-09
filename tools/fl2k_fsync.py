#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Explore the frame_sync bit (0x8004 bit 1) against vertical timing values.

Run as root with the fl2000 module loaded (vga_mode=1) and fl2k_mode built next to this script.
The vertical registers are rewritten live over a 320x240 15 kHz mode, with and without frame_sync,
over a grid of odd and even values, both field-like (262/263) and frame-like (524/525 with
vactive 480, as an interlaced modeline describes them).

For each combination the per-frame completion timestamps (debugfs "ring") are analysed:
  mean     mean period in lines
  alt      mean period of even-indexed frames minus odd-indexed ones, in lines: a value near
           +/-1 means fields alternate between N and N+1 lines, i.e. interlace-like timing
  pattern  mean period per position for cycles of 2, 3 and 4 frames
"""

import argparse
import glob
import itertools
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MODE = "6.700 320 336 368 426 240 244 247 262 -hsync -vsync"


class Chip:
    def __init__(self):
        dirs = glob.glob("/sys/kernel/debug/fl2000-*")
        if len(dirs) != 1:
            sys.exit(f"expected exactly one fl2000 debugfs directory, found {dirs}")
        self.d = dirs[0]

    def read(self, reg):
        with open(os.path.join(self.d, "reg"), "w") as f:
            f.write(f"0x{reg:x}")
        with open(os.path.join(self.d, "reg")) as f:
            return int(f.read().split()[1], 16)

    def write(self, reg, val):
        with open(os.path.join(self.d, "reg"), "w") as f:
            f.write(f"0x{reg:x} 0x{val:x}")

    def deltas(self, seconds):
        with open(os.path.join(self.d, "frames"), "w") as f:
            f.write("0")
        time.sleep(seconds)
        with open(os.path.join(self.d, "ring")) as f:
            ts = [int(x) for x in f.read().split()]
        return [b - a for a, b in zip(ts, ts[1:])]


def analyse(dts, line_ns):
    if len(dts) < 8:
        return f"STALLED ({len(dts)} frames)"
    lines = [d / line_ns for d in dts]
    mean = sum(lines) / len(lines)
    even = lines[0::2]
    odd = lines[1::2]
    alt = sum(even) / len(even) - sum(odd) / len(odd)
    pats = []
    for k in (2, 3, 4):
        means = [sum(lines[i::k]) / len(lines[i::k]) for i in range(k)]
        pats.append("/".join(f"{m:.2f}" for m in means))
    return f"mean {mean:8.3f}  alt {alt:+6.3f}  n={len(lines):4d}  k2 {pats[0]}  k3 {pats[1]}  k4 {pats[2]}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--seconds", type=float, default=3.0)
    args = ap.parse_args()

    chip = Chip()
    proc = subprocess.Popen([os.path.join(HERE, "fl2k_mode"), "-p", "lines", *MODE.split()],
                            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(2)
    if proc.poll() is not None:
        sys.exit("fl2k_mode failed")

    p0, v0, s0 = chip.read(0x8004), chip.read(0x8010), chip.read(0x8014)
    try:
        base = chip.deltas(args.seconds * 2)
        line_ns = sum(base) / len(base) / 262
        print(f"line {line_ns:.1f} ns (from the 262 line baseline)", flush=True)

        vsync_w = (s0 >> 16) & 0xF
        grid = itertools.product(
            (True, False),                                   # frame_sync
            ((262, 240), (263, 240), (524, 480), (525, 480)),  # (vtotal, vactive)
            ((20, 20), (21, 21), (20, 19), (21, 20), (40, 40), (41, 41)),  # (vstart, latency)
        )
        for fs, (vt, va), (vs, lat) in grid:
            chip.write(0x8004, (p0 | 2) if fs else (p0 & ~2))
            chip.write(0x8010, (va << 16) | vt)
            chip.write(0x8014, (lat << 20) | (vsync_w << 16) | vs)
            time.sleep(0.3)
            res = analyse(chip.deltas(args.seconds), line_ns)
            print(f"fs={int(fs)} vtotal={vt} vactive={va} vstart={vs:2d} lat={lat:2d}  {res}",
                  flush=True)
    finally:
        chip.write(0x8004, p0)
        chip.write(0x8010, v0)
        chip.write(0x8014, s0)
        proc.stdin.write("\n")
        proc.stdin.flush()
        proc.wait(timeout=5)


if __name__ == "__main__":
    main()
