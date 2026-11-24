import sys,glob,re
# usage: segdraws.py rundir... : draws/frame and GPU ms/frame in 10 segments of the bench window (perf.tsv aligned on "[bench] window start at frame N")
for d in sys.argv[1:]:
    log=open(glob.glob(d+'/run/game.log')[0],errors='replace').read()
    m=re.search(r'\[bench\] window start at frame (\d+)',log)
    if not m: print(d,'no window'); continue
    s=int(m.group(1)); seg=[[0,0,0.0] for _ in range(10)]
    for l in open(d+'/run/perf.tsv'):
        a=l.split('\t')
        if not a[0].isdigit(): continue
        f=int(a[0])-s
        if 0<=f<3600:
            g=seg[f//360]; g[0]+=1; g[1]+=int(a[7] or 0)
            try: g[2]+=float(a[4])
            except ValueError: pass
    print('%-8s'%d.split('/')[-1],' '.join('%5.0f'%(g[1]/max(g[0],1)) for g in seg),' | gpu',' '.join('%5.2f'%(g[2]/max(g[0],1)) for g in seg))
