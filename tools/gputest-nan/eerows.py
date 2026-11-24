#!/usr/bin/env python3
"""eerows.py OUT DRAW: per colour readback of DRAW, shape and row ranges fully/partly 0xEE."""
import sys, json, numpy as np
out, want = sys.argv[1], int(sys.argv[2])
for l in open(out+'/draws.jsonl'):
    d = json.loads(l)
    if d['draw'] != want: continue
    for t in d['targets']:
        if not t.get('file'): continue
        u = np.load(out+'/'+t['file']).view(np.uint32)
        ee = (u == 0xEEEEEEEE).all(-1) if u.ndim == 3 else (u == 0xEEEEEEEE)
        r = ee.sum(1); full = np.where(r == ee.shape[1])[0]; part = np.where((r > 0) & (r < ee.shape[1]))[0]
        def runs(a):
            if not len(a): return []
            s = [[a[0], a[0]]]
            for x in a[1:]:
                if x == s[-1][1]+1: s[-1][1] = x
                else: s.append([x, x])
            return s
        print(t['target'], t['key'], u.shape, 'full', runs(full)[:8], 'part', runs(part)[:8])
