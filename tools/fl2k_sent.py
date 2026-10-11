#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Analyse what the FL2000 actually sent, frame by frame (debugfs "sent").

Run as root while something updates the display, for example:

    sudo ./fl2k_mode -a 50 -t 8 6.7596 320 352 383 430 240 241 244 262 -hsync -vsync &
    sleep 3; sudo ./fl2k_sent.py

Reports how many sent frames were new, repeats of the previous one, or older than a frame
already shown (a step back in time, seen as flicker), how many rendered frames never made it to
the wire, and how long new frames waited between conversion and submission to the chip. Add the
queue depth (module parameter queue_depth) times the frame period for the time until they are
on screen.
"""

import glob
import sys


def main():
    dirs = glob.glob("/sys/kernel/debug/fl2000-*")
    if len(dirs) != 1:
        sys.exit(f"expected exactly one fl2000 debugfs directory, found {dirs}")

    with open(dirs[0] + "/sent") as f:
        header = f.readline().split()
        rows = [line.split() for line in f]

    rendered, dropped, superseded = int(header[2]), int(header[4]), int(header[6])
    frames = [(int(ns), int(seq), h, kind) for ns, seq, h, kind, _ in rows]
    waits = sorted((int(r[0]) - int(r[4])) / 1e6 for r in rows if r[3] == "new" and int(r[4]))
    if len(frames) < 2:
        sys.exit("not enough frames")

    new = sum(1 for f in frames if f[3] == "new")
    rep = len(frames) - new
    back = 0
    max_seq = 0
    examples = []
    for i, (ns, seq, h, kind) in enumerate(frames):
        if seq < max_seq:
            back += 1
            if len(examples) < 5:
                examples.append(" ".join(str(f[1]) for f in frames[max(0, i - 4):i + 2]))
        max_seq = max(max_seq, seq)

    seqs = [f[1] for f in frames if f[3] == "new"]
    skipped = sum(b - a - 1 for a, b in zip(seqs, seqs[1:]) if b > a + 1)
    span = (frames[-1][0] - frames[0][0]) / 1e9

    print(f"window {span:.2f} s, {len(frames)} frames sent ({len(frames) / span:.2f}/s)")
    print(f"  new {new}, repeated {rep}, back in time {back}")
    print(f"  rendered so far {rendered}, dropped (no free buffer) {dropped}, "
          f"superseded by a newer frame {superseded}, never sent in this window {skipped}")
    if waits:
        print(f"  ready -> submitted: median {waits[len(waits) // 2]:.2f} ms, "
              f"max {waits[-1]:.2f} ms")
    for e in examples:
        print(f"  step back: ... {e}")


if __name__ == "__main__":
    main()
