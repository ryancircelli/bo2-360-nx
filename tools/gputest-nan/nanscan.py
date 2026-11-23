#!/usr/bin/env python3
"""Per-draw NaN/Inf scan of a rexgpu-replay output dir (draws.jsonl + draws/*.npy, delta readbacks).

usage: nanscan.py OUTDIR [--json out.json] [--top N]
A draw "writes NaN" when a colour target pixel inside its changed rect is non-finite now and was finite
(or unknown) before. Reports per draw, per (vs,ps) group, and non-finite pixels left in each surface at
the end of the replay (what later draws / the frame could read).
"""
import json, sys, collections, argparse
from pathlib import Path
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--json')
ap.add_argument('--top', type=int, default=25)
a = ap.parse_args()
out = Path(a.out)
surf = {}  # (label,key,w,h) -> full image float32
draws = []
groups = collections.defaultdict(lambda: {'draws': [], 'new': 0})
for line in open(out / 'draws.jsonl'):
    d = json.loads(line)
    if d.get('is_resolve'):
        continue
    for t in d.get('targets', []):
        if t['channels'] != 4:
            continue
        k = (t['target'], t['key'], t['width'], t['height'])
        x, y, w, h = t['rect']
        prev = surf.get(k)
        if t['base'] == 'full' or prev is None:
            prev_crop = None
        if t['file'] is None:
            continue
        arr = np.load(out / t['file'])
        if t['base'] == 'full':
            full = arr.copy()
            if prev is not None and prev.shape == full.shape:
                before = prev
            else:
                before = None
        else:
            if prev is None:
                continue  # cannot reconstruct
            before = prev
            full = prev.copy()
            full[y:y + h, x:x + w] = arr
        nf_now = ~np.isfinite(full[y:y + h, x:x + w]).all(axis=-1)
        if before is not None:
            nf_before = ~np.isfinite(before[y:y + h, x:x + w]).all(axis=-1)
            new = int((nf_now & ~nf_before).sum())
        else:
            new = int(nf_now.sum())
        surf[k] = full
        if new:
            nan = int(np.isnan(full[y:y + h, x:x + w]).any(axis=-1).sum())
            draws.append({'draw': d['draw'], 'vs': d['vs'], 'ps': d['ps'], 'target': t['target'], 'key': t['key'],
                          'new_nonfinite': new, 'nan_px': nan, 'rect': t['rect'],
                          'depthctl': d.get('regs', {}).get('RB_DEPTHCONTROL'),
                          'blend0': d.get('regs', {}).get('RB_BLENDCONTROL0')})
            g = groups[(d['vs'], d['ps'])]
            g['draws'].append(d['draw'])
            g['new'] += new
left = []
for k, img in surf.items():
    n = int((~np.isfinite(img).all(axis=-1)).sum())
    if n:
        left.append({'target': k[0], 'key': k[1], 'w': k[2], 'h': k[3], 'nonfinite_px': n})
res = {'out': str(out), 'nan_draws': len(draws), 'nan_groups': len(groups),
       'groups': sorted([{'vs': k[0], 'ps': k[1], 'draws': len(v['draws']), 'first': v['draws'][0],
                          'list': v['draws'], 'new_px': v['new']} for k, v in groups.items()],
                        key=lambda g: -g['draws']),
       'draws': draws, 'surfaces_left_nonfinite': left}
print(f"{out.name}: nan_draws={len(draws)} groups={len(groups)} surfaces_with_nonfinite_at_end={len(left)}")
for g in res['groups'][:a.top]:
    print(f"  ps {g['ps']} vs {g['vs']} draws {g['draws']} first {g['first']} new_px {g['new_px']}")
for s in left[:a.top]:
    print(f"  left: {s}")
if a.json:
    json.dump(res, open(a.json, 'w'), indent=1)
