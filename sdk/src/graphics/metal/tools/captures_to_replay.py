#!/usr/bin/env python3
# Copyright (c) 2026 ReXGlue contributors. BSD-3-Clause, see the SDK LICENSE.
"""Turn a folder of guest output screenshots (PNG) into a gputest replay folder.

Any renderer that can save its final frames as PNG (a title's own capture code,
the MoltenVK path, a native renderer that cannot load GPU plugins) can then be
compared with a Metal plugin frame dump (metal_frame_dump) or another capture
by tools/gputest/replay/replay_diff.py:

    captures_to_replay.py <png folder> <out folder> [--backend NAME]
    replay_diff.py diff <metal dump> <out folder> -o report

Each PNG becomes one record in draws.jsonl with the single target "frame",
in file name order. The frame number is taken from the first number in the
file name after an optional "frame"/"capture" prefix, or the file's index.
Needs numpy and Pillow, like replay_diff.py.
"""

import argparse
import json
import os
import re
import sys

import numpy as np
from PIL import Image


def xxh3_or_none(data):
    try:
        import xxhash  # optional, matches the hashes the C++ writers use
    except ImportError:
        return None
    return "%016X" % xxhash.xxh3_64_intdigest(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("captures")
    ap.add_argument("out")
    ap.add_argument("--backend", default=None,
                    help="label for summary.json (default: the folder name)")
    ap.add_argument("--title-id", default="")
    args = ap.parse_args()

    names = sorted(n for n in os.listdir(args.captures)
                   if n.lower().endswith(".png"))
    if not names:
        print("no PNG files in", args.captures, file=sys.stderr)
        return 1
    os.makedirs(os.path.join(args.out, "draws"), exist_ok=True)
    frames = []
    with open(os.path.join(args.out, "draws.jsonl"), "w") as jl:
        for index, name in enumerate(names):
            image = Image.open(os.path.join(args.captures, name)).convert("RGBA")
            pixels = np.asarray(image, dtype=np.float32) / 255.0
            pixels[..., 3] = 1.0
            height, width = pixels.shape[:2]
            m = re.search(r"(\d+)", name)
            frame = int(m.group(1)) if m else index
            file = "draws/%06d_frame.npy" % index
            np.save(os.path.join(args.out, file), pixels)
            record = {
                "draw": index,
                "frame": frame,
                "packet": None, "prim_type": None, "index_count": None,
                "succeeded": True, "is_resolve": False,
                "vs": None, "ps": None, "regs": {},
                "source_file": name,
                "targets": [{
                    "target": "frame",
                    "key": "frame_%d_%d" % (width, height),
                    "width": width,
                    "height": height,
                    "channels": 4,
                    "format": "rgba32f",
                    "hash": xxh3_or_none(pixels.tobytes()),
                    "base": "full",
                    "file": file,
                    "rect": [0, 0, width, height],
                }],
            }
            jl.write(json.dumps(record) + "\n")
            frames.append(frame)
    summary = {
        "source": "png",
        "backend": args.backend or os.path.basename(os.path.normpath(args.captures)),
        "title_id": args.title_id,
        "frames": len(frames),
        "draws": len(frames),
        "ok": True,
    }
    with open(os.path.join(args.out, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print("%d frames -> %s" % (len(frames), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
