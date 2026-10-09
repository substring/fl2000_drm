#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Measure the real frame period of FL2000 modes and compare it with the configured timings.

Run as root with the fl2000 module loaded (vga_mode=1) and fl2k_mode built next to this script:

    sudo ./fl2k_measure.py [-s SECONDS] [MODELINE...]

Each MODELINE is one quoted string in fl2k_mode syntax. Without any, a built-in set of
progressive VGA and 15 kHz modes is measured. The expected period comes from the mode the driver
actually programmed ("Mode configured" in the kernel log), so PLL rounding and htotal
adjustments are accounted for.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

DEFAULT_MODES = [
    # VGA references
    "25.175 640 656 752 800 480 490 492 525 -hsync -vsync",
    "65.000 1024 1048 1184 1344 768 771 777 806 -hsync -vsync",
    # 15 kHz progressive
    "6.700 320 336 368 426 240 243 246 262 -hsync -vsync",
    "13.500 640 664 728 858 240 243 246 262 -hsync -vsync",
    "52.000 2560 2624 2864 3328 240 243 246 262 -hsync -vsync",
]

CONFIGURED = re.compile(r'Mode configured: "[^"]*": (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)')


def debugfs_dir():
    dirs = glob.glob("/sys/kernel/debug/fl2000-*")
    if len(dirs) != 1:
        sys.exit(f"expected exactly one fl2000 debugfs directory, found {dirs}")
    return dirs[0]


def read_frames(d):
    out = {}
    with open(os.path.join(d, "frames")) as f:
        for line in f:
            parts = line.split()
            for k, v in zip(parts[0::2], parts[1::2]):
                out[k] = float(v)
    return out


def last_configured_mode():
    log = subprocess.run(["dmesg"], capture_output=True, text=True).stdout
    matches = CONFIGURED.findall(log)
    if not matches:
        return None
    _, clock, hd, hss, hse, ht, vd, vss, vse, vt = map(int, matches[-1])
    return clock, ht, vt


def measure(modeline, seconds, d):
    proc = subprocess.Popen([os.path.join(HERE, "fl2k_mode"), "-p", "grid", *modeline.split()],
                            stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        time.sleep(1.5)  # let the pipeline fill
        if proc.poll() is not None:
            return f"fl2k_mode failed: {proc.stderr.read().strip()}"
        with open(os.path.join(d, "frames"), "w") as f:
            f.write("0")
        time.sleep(seconds)
        stats = read_frames(d)
        # Read while the mode is still held: fbdev restores its own mode on exit
        conf = last_configured_mode()
    finally:
        proc.stdin.write("\n")
        proc.stdin.flush()
        proc.wait(timeout=5)

    if not conf or stats.get("count", 0) < 3:
        return f"no data (count={stats.get('count')}, configured={conf})"

    clock_khz, ht, vt = conf
    expected_ns = ht * vt / clock_khz * 1e6
    measured_ns = stats["mean_period_ns"]
    err_ppm = (measured_ns - expected_ns) / expected_ns * 1e6
    return (f"configured clk={clock_khz}kHz htot={ht} vtot={vt} expected={expected_ns:.1f}ns "
            f"({1e9 / expected_ns:.4f}Hz)\n"
            f"    measured={measured_ns:.1f}ns ({1e9 / measured_ns:.4f}Hz) err={err_ppm:+.0f}ppm "
            f"frames={int(stats['count'])} jitter: min={stats.get('min_ns', 0):.0f} "
            f"max={stats.get('max_ns', 0):.0f} stddev={stats.get('stddev_ns', 0):.0f}ns")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--seconds", type=float, default=5.0)
    ap.add_argument("modes", nargs="*")
    args = ap.parse_args()

    d = debugfs_dir()
    for modeline in args.modes or DEFAULT_MODES:
        print(f"{modeline}\n    {measure(modeline, args.seconds, d)}", flush=True)


if __name__ == "__main__":
    main()
