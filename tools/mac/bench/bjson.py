import json,sys,glob,re,collections
# usage: bjson.py rundir... : per-thread M instr/frame from the "[bench] json" log line
rows={};tot={}
for d in sys.argv[1:]:
    L=[l for f in glob.glob(d+'/**/game.log',recursive=True) for l in open(f,errors='replace') if '[bench] json ' in l]
    if not L: print(d,'no bench json'); continue
    j=json.loads(L[-1].split('[bench] json ',1)[1]); n=j['frames']; k=d.rstrip('/').split('/')[-1]
    tot[k]=(j['fps'],j['instr']/n/1e6,j['cpu_ms']/n,j['gpu_ms']/n,j['stall_frames'])
    agg=collections.Counter()
    for t in j['threads']:
        nm=re.sub(r'\s*\d+$','',re.sub(r'\s*\(.*','',t['name'] or 'unnamed'))
        agg[nm]+=t['instr']/n/1e6
    rows[k]=agg
ks=list(tot)
print('%-28s'%'',*['%9s'%k for k in ks])
for i,lab in enumerate(['fps','instr M/f','cpu ms/f','gpu ms/f','stalls']): print('%-28s'%lab,*['%9.2f'%tot[k][i] for k in ks])
names=sorted({n for r in rows.values() for n in r},key=lambda n:-max(rows[k].get(n,0) for k in ks))
for nm in names[:int(os_n) if (os_n:=0) else 22]: print('%-28s'%nm[:28],*['%9.1f'%rows[k].get(nm,0) for k in ks])
