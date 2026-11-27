import json,sys,glob,re,collections
# usage: rt.py rundir... : every "[bench] json" round: work and per-thread work (instr - spin - gap_spin) M/frame
for d in sys.argv[1:]:
    L=[l for f in glob.glob(d+'/run/game.log')+glob.glob(d+'/run/logs/bo2zm-*.log') for l in open(f,errors='replace') if '[bench] json ' in l]
    seen=set()
    for l in L:
        j=json.loads(l.split('[bench] json ',1)[1]); n=j['frames']
        if (j.get('round'),j['instr']) in seen: continue
        seen.add((j.get('round'),j['instr']))
        agg=collections.Counter()
        for t in j['threads']:
            nm=re.sub(r'\s*\d+$','',re.sub(r'\s*\(.*','',t['name'] or 'unnamed'))
            agg[nm[:10]]+=(t['instr']-t.get('spin_instr',0)-t.get('gap_spin',0))/n/1e6
        print('%-9s r%s fps %5.1f work %6.1f gpu %5.2f |'%(d.split('/')[-1],j.get('round'),j['fps'],j['work']/n/1e6,j['gpu_frame_ms_p50']),' '.join('%s=%.0f'%kv for kv in agg.most_common(8)))
