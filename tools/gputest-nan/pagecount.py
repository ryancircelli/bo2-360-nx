#!/usr/bin/env python3
"""Count all-0xFF vs all-0xEE pixels in every colour readback of a replay out dir
(REX_GPU_TRACE_POISON_GPU=1 runs: 0xEE = resolve skipped the page, 0xFF = guest poison uploaded).
usage: pagecount.py OUTDIR"""
import sys, json, numpy as np
out=sys.argv[1]; tot={}
for l in open(out+'/draws.jsonl'):
    d=json.loads(l)
    for t in d['targets']:
        if t['target']=='depth' or not t.get('file'): continue
        u=np.load(out+'/'+t['file']).view(np.uint32)
        ff=int((u==0xFFFFFFFF).all(-1).sum()); ee=int((u==0xEEEEEEEE).all(-1).sum())
        if ff or ee: print(d['draw'],d['ps'],t['key'],'ff',ff,'ee',ee)
