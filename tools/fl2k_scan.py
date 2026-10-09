#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Scan register bits of a live FL2000 mode and report which ones change the output timing.

Run as root with the fl2000 module loaded (vga_mode=1), fl2k_mode built next to this script and
NO screen attached: bits are flipped blindly and may produce any signal.

    sudo ./fl2k_scan.py [-s SECONDS] [REG:MASK ...]

Each bit set in MASK is toggled on its own, measured, then restored. Without arguments the
reserved and undocumented bits of the timing generator registers are scanned. Timing writes take
effect live, so the frame period (from stream URB completions) and the pixel clock counter
(0x8040, one second window) reveal any timing change. An interlaced output would show up as a
period change of about half a line (1900 ppm on a 262 line mode).

With --modeset, each bit is instead forced through the driver's debugfs "pokes" list and a full
modeset is done for it, reset included. That catches bits which only latch on a chip reset, as
the PLL does, at the cost of a few seconds per bit.
"""

import argparse
import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

MODE = "6.700 320 336 368 426 240 243 246 262 -hsync -vsync"

# Bits nobody uses: reserved fields of the timing registers plus the documented-but-unused
# control bits of 0x8004 (colour modes, CCS reset and DAC/timing enables left out)
DEFAULT_SCAN = {
    0x8004: 0x70fff83e & ~0x000001c0,
    0x8008: 0xf000f000,
    0x800c: 0xfe00f000,
    0x8010: 0xf000f000,
    0x8014: 0xc000f000,
    0x8018: 0xffffffff,
}

THRESHOLD_PPM = 200


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

    def pokes(self, line):
        with open(os.path.join(self.d, "pokes"), "w") as f:
            f.write(line)

    def period(self, seconds):
        with open(os.path.join(self.d, "frames"), "w") as f:
            f.write("0")
        time.sleep(seconds)
        out = {}
        with open(os.path.join(self.d, "frames")) as f:
            for line in f:
                parts = line.split()
                for k, v in zip(parts[0::2], parts[1::2]):
                    out[k] = float(v)
        return out.get("mean_period_ns"), int(out.get("count", 0))


def start_mode():
    proc = subprocess.Popen([os.path.join(HERE, "fl2k_mode"), "-p", "lines", *MODE.split()],
                            stdin=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    time.sleep(2)
    if proc.poll() is not None:
        sys.exit("fl2k_mode failed")
    return proc


def stop_mode(proc):
    try:
        proc.stdin.write("\n")
        proc.stdin.flush()
        proc.wait(timeout=5)
    except (BrokenPipeError, subprocess.TimeoutExpired):
        proc.kill()


def scan_modeset(chip, scan, seconds):
    chip.pokes("clear")
    proc = start_mode()
    try:
        base, _ = chip.period(seconds * 2)
        orig = {reg: chip.read(reg) for reg in scan}
    finally:
        stop_mode(proc)
    print(f"baseline period {base:.0f} ns (modeset per bit)", flush=True)

    for reg, mask in scan.items():
        print(f"0x{reg:04x} = 0x{orig[reg]:08x}", flush=True)
        for bit in range(32):
            if not mask & (1 << bit):
                continue
            b = 1 << bit
            chip.pokes("clear")
            chip.pokes(f"0x{reg:x} 0x{0 if orig[reg] & b else b:x} 0x{b if orig[reg] & b else 0:x}")
            proc = start_mode()
            try:
                now = chip.read(reg)
                per, count = chip.period(seconds)
            finally:
                stop_mode(proc)
                chip.pokes("clear")
                # The driver never rewrites reserved bits: put the original value back by hand
                cur = chip.read(reg)
                chip.write(reg, (cur & ~b) | (orig[reg] & b))
            held = "held" if (now ^ orig[reg]) & b else f"lost(0x{now:08x})"
            if per is None or count < 3:
                verdict = f"STREAM STALLED (count={count})"
            else:
                ppm = (per - base) / base * 1e6
                verdict = f"{ppm:+8.0f} ppm" + ("  <== TIMING CHANGE" if abs(ppm) > THRESHOLD_PPM else "")
            print(f"  bit {bit:2d} {held:16s} {verdict}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--seconds", type=float, default=1.5)
    ap.add_argument("--modeset", action="store_true", help="full modeset per bit")
    ap.add_argument("targets", nargs="*", help="REG:MASK in hex")
    args = ap.parse_args()

    scan = DEFAULT_SCAN
    if args.targets:
        scan = {int(r, 16): int(m, 16) for r, m in (t.split(":") for t in args.targets)}

    chip = Chip()
    if args.modeset:
        return scan_modeset(chip, scan, args.seconds)
    proc = start_mode()
    try:
        base, _ = chip.period(args.seconds * 2)
        pix = chip.read(0x8040)
        print(f"baseline period {base:.0f} ns, pixel count {pix}", flush=True)

        for reg, mask in scan.items():
            orig = chip.read(reg)
            print(f"0x{reg:04x} = 0x{orig:08x}", flush=True)
            for bit in range(32):
                if not mask & (1 << bit):
                    continue
                val = orig ^ (1 << bit)
                chip.write(reg, val)
                back = chip.read(reg)
                time.sleep(0.2)
                per, count = chip.period(args.seconds)
                time.sleep(max(0.0, 1.1 - args.seconds))  # let 0x8040 cover a full window
                pixc = chip.read(0x8040)
                status = chip.read(0x8000)
                chip.write(reg, orig)

                sticky = "rw" if back == val else f"ro(0x{back:08x})"
                if per is None or count < 3:
                    verdict = f"STREAM STALLED (count={count})"
                else:
                    ppm = (per - base) / base * 1e6
                    verdict = f"{ppm:+8.0f} ppm" + ("  <== TIMING CHANGE" if abs(ppm) > THRESHOLD_PPM else "")
                pix_ppm = (pixc - pix) / pix * 1e6 if pix else 0
                pixnote = f" pixclk {pix_ppm:+.0f}ppm" if abs(pix_ppm) > THRESHOLD_PPM else ""
                print(f"  bit {bit:2d} {sticky:16s} {verdict}{pixnote} status=0x{status:08x}",
                      flush=True)

                # Make sure the restore really brought the baseline back
                time.sleep(0.3)
                per, count = chip.period(1.0)
                if per is None or count < 3 or abs(per - base) / base * 1e6 > THRESHOLD_PPM:
                    print("  !! baseline lost, re-setting the mode", flush=True)
                    stop_mode(proc)
                    proc = start_mode()
    finally:
        stop_mode(proc)


if __name__ == "__main__":
    main()
