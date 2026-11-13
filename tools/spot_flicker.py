#!/usr/bin/env python3
"""Finds what blinks in a test spot's burst of screenshots (bo2zm_test_spot, games/bo2/src/bo2zm_benchmark.cpp).

The test stands the player still with no zombies, so in a correct image almost nothing changes from one screenshot to
the next. This compares the burst pixel by pixel:
  - changed: the pixel's colour moved by more than --threshold (0-255, any channel) at least once;
  - blinking: it moved that much in two or more separate steps (it went one way and came back, or kept jumping),
    which is what a flickering light or a surface that drops in and out does, unlike a slow fade.
It prints the share of each, the frames that differ most from the others (a whole-image flash), and the screen areas
(a grid of 40x40 cells on 1280x720) with the most blinking pixels; --out writes the first screenshot, darkened, with
blinking pixels red and changed ones yellow.

The HUD (points, the round, the ammo, the compass) can change on its own: read those areas with that in mind.
The screenshots and the picture come from the game: keep them out of the repo.

usage: python -I tools/spot_flicker.py <captures folder> [--burst spot_zm_buried_20261022-171500] [--out map.png]
       (without --burst: the newest spot_* burst in the folder)
"""

import argparse
import os
import re
import sys

import numpy as np
from PIL import Image

CELL = 40


def find_burst(folder, burst):
    """The burst's screenshots, in order: <burst>_NN.png."""
    # Test spots' bursts (spot_<map>_<time>_NN) and MARK LOCATION's four screenshots (location_NN_<time>_N).
    names = [n for n in os.listdir(folder) if re.match(r"(spot|location)_.+_\d{8}-\d{6}_\d{1,2}\.png$", n)]
    if not names:
        sys.exit(f"spot_flicker: no spot_*.png or location_*.png in {folder}")
    if burst is None:
        newest = max(names, key=lambda n: os.path.getmtime(os.path.join(folder, n)))
        burst = newest.rsplit("_", 1)[0]
    frames = sorted(n for n in names if n.rsplit("_", 1)[0] == burst)
    if len(frames) < 3:
        sys.exit(f"spot_flicker: {burst} has {len(frames)} screenshots; at least 3 are needed")
    return burst, [os.path.join(folder, n) for n in frames]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--burst", help="the burst's name, without _NN.png (default: the newest)")
    ap.add_argument("--threshold", type=int, default=24, help="a change in any channel above this counts (0-255)")
    ap.add_argument("--out", help="write the map of what changed and blinked here (PNG)")
    ap.add_argument("--cells", type=int, default=12, help="how many of the busiest screen areas to list")
    a = ap.parse_args()

    burst, paths = find_burst(a.folder, a.burst)
    frames = np.stack([np.asarray(Image.open(p).convert("RGB"), dtype=np.int16) for p in paths])  # n, h, w, 3
    n, h, w, _ = frames.shape

    # Steps between consecutive screenshots that move a pixel by more than the threshold, in any channel.
    steps = np.abs(np.diff(frames, axis=0)).max(axis=3) > a.threshold  # n-1, h, w
    jumps = steps.sum(axis=0)
    changed = jumps >= 1
    blinking = jumps >= 2
    total = h * w
    print(f"{burst}: {n} screenshots of {w}x{h}, threshold {a.threshold}")
    print(f"  changed:  {changed.sum():8d} pixels ({100.0 * changed.sum() / total:.3f} %)")
    print(f"  blinking: {blinking.sum():8d} pixels ({100.0 * blinking.sum() / total:.3f} %)")

    # Each screenshot against the median of the burst: one far from it is a whole-image flash.
    median = np.median(frames, axis=0)
    off = np.abs(frames - median).max(axis=3) > a.threshold
    shares = off.reshape(n, -1).mean(axis=1) * 100.0
    print("  pixels away from the burst's median, per screenshot (%): "
          + " ".join(f"{i + 1}:{s:.2f}" for i, s in enumerate(shares)))

    # The busiest screen areas.
    cells = []
    for y in range(0, h, CELL):
        for x in range(0, w, CELL):
            count = int(blinking[y:y + CELL, x:x + CELL].sum())
            if count:
                cells.append((count, x, y))
    cells.sort(reverse=True)
    if cells:
        print(f"  areas with the most blinking pixels (x, y of the {CELL}x{CELL} cell's top left):")
        for count, x, y in cells[:a.cells]:
            print(f"    {x:4d},{y:3d}  {count:5d} pixels ({100.0 * count / (CELL * CELL):.0f} % of the cell)")
    else:
        print("  no blinking area")

    if a.out:
        picture = (frames[0] * 0.35).astype(np.uint8)
        picture[changed] = (255, 220, 0)
        picture[blinking] = (255, 0, 0)
        Image.fromarray(picture).save(a.out)
        print(f"  map: {a.out} (red blinking, yellow changed)")


if __name__ == "__main__":
    main()
