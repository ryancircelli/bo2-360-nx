#!/usr/bin/env python3
"""Add LIGHT callback-success probes to a PRIVATE generated copy (never follow app/generated links).
Run after codegen/direct_calls. Five exact job callback return sites; failed/put-back calls only test r3.
"""
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
gen=ROOT/'app/generated'
if gen.is_symlink() or gen.resolve()!=gen.absolute():
    raise SystemExit('LIGHT probes require a private, non-symlink app/generated directory')
sites={'8284B0A8','8284B12C','8284B3B4','8284B85C','8284B8E0'}
seen=set()
for p in (gen/'default_mp').glob('*recomp.*.cpp'):
    s=p.read_text(); old=s
    for site in sites:
        needle=f'ctx.lr = 0x{site};\n\tREX_CALL_INDIRECT_FUNC(ctr.u32);'
        if needle not in s: continue
        seen.add(site)
        probe='\n\tif (ctx.r3.u32 == 0 && bo2zm_critpath_light) Bo2LightJobComplete(base, ctx.r1.u32 + 96);'
        if needle+probe not in s: s=s.replace(needle,needle+probe)
    if s!=old:
        if '#include "../../src/bo2zm_critpath_light.h"' not in s: s='#include "../../src/bo2zm_critpath_light.h"\n'+s
        p.write_text(s)
if seen!=sites: raise SystemExit(f'missing callback return sites: {sites-seen}')
print('LIGHT probes: five callback-return sites verified')
