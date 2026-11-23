#!/usr/bin/env python3
"""Classify the non-finite pixels each NaN-origin draw (from nanscan json) leaves in its readback:
poison = all four channels 0xFFFFFFFF (rexgpu-replay fills the readback destination with 0xFF before its resolve;
these pixels were never written by the resolve); other = real non-finite float values.
Also computes the EDRAM tile span of the readback (base + rows*pitch tiles) vs the 2048-tile EDRAM.
usage: classify.py OUTDIR NANJSON"""
import json, sys, numpy as np
out, nj = sys.argv[1], json.load(open(sys.argv[2]))
recs = {}
for l in open(out + '/draws.jsonl'):
    d = json.loads(l); recs[d['draw']] = d
for e in nj['draws']:
    d = recs[e['draw']]; t = [t for t in d['targets'] if t['target'] == e['target']][0]
    # reconstruct is done by nanscan; here only the changed crop is classified
    a = np.load(out + '/' + t['file']); u = a.view(np.uint32)
    bad = ~np.isfinite(a).all(-1); pois = (u == 0xFFFFFFFF).all(-1)
    base, fmt, pitch, msaa = [int(x) for x in t['key'].split('_')[1:]]
    sw, sh = (80, 16)
    sx, sy = {0: (1, 1), 1: (1, 2), 2: (2, 2)}[msaa]
    bpp64 = fmt in (5, 7, 15)  # 16_16_16_16, 16_16_16_16_FLOAT, 32_32_FLOAT
    tw = (sw // (2 if bpp64 else 1)) // sx; th = sh // sy
    tiles_row = -(-pitch // tw); rows = -(-t['height'] // th)
    span = base + tiles_row * rows
    print(f"draw {e['draw']} ps {d['ps']} key {t['key']} {t['width']}x{t['height']} rect {t['rect']} nonfinite {int(bad.sum())} "
          f"poison {int(pois.sum())} other {int((bad & ~pois).sum())} edram_tiles {base}..{span}{' EXCEEDS 2048' if span > 2048 else ''}")
