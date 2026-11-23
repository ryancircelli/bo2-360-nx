#!/usr/bin/env python3
"""Map poisoned pixels of one readback back to byte offsets in the tiled resolve destination
(same GetTiledOffset2D as frame_replay.cpp, 128bpp colour) and print the poisoned byte ranges.
usage: poisonbytes.py NPY"""
import sys, numpy as np
a=np.load(sys.argv[1]); u=a.view(np.uint32); H,W=a.shape[:2]
pitch=(W+31)&~31; hal=(H+31)&~31; bppl=4
def off(x,y):
    # texture_util::GetTiledOffset2D (Xenia)
    pitch_t=(pitch+31)&~31
    macro=((x>>5)+(y>>5)*(pitch_t>>5))<<(bppl+7)
    micro=((x&7)+((y&0xE)<<2))<<bppl
    offset=macro+((micro&~0xF)<<1)+(micro&0xF)+((y&1)<<4)
    return ((offset&~0x1FF)<<3)+((y&16)<<7)+((offset&0x1C0)<<2)+(((((y&8)>>2)+(x>>3))&3)<<6)+(offset&0x3F)
p=(u==0xFFFFFFFF).all(-1); ys,xs=np.nonzero(p)
o=np.sort(np.array([off(x,y) for y,x in zip(ys,xs)]))
total=pitch*hal*16
print('image',W,H,'dest bytes',total,'poison px',len(o))
if len(o):
    br=np.nonzero(np.diff(o)!=16)[0]; starts=np.r_[o[0],o[br+1]]; ends=np.r_[o[br],o[-1]]+16
    for s,e in list(zip(starts,ends))[:12]: print(f'  {s:#x}..{e:#x} ({e-s} B) page {s>>12:#x}..{(e-1)>>12:#x}')
    print('ranges',len(starts))
