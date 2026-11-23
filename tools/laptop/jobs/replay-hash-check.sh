#!/bin/bash
# runq laptop job: replay the town4 fence trace with the Linux refxenos build and compare every target hash with the
# Mac baseline (3646 hashes). CPU only. Exit 0 only when no hash differs and the draw ids match.
# usage: runq submit --where laptop --name X --out DIR -- bash ~/bo2-worker/jobs/replay-hash-check.sh {OUT}
set -u
root=$HOME/bo2-worker/drawer-linux
out=${1:?out dir}
"$root/ref-build/refxenos" replay "$root/415608C3_5467.xtr" -o "$out/cpu-frame0" --rop edram --frame 0 > "$out/replay.log" 2>&1
echo "replay exit $?" > "$out/summary.txt"
python3 - "$root" "$out" >> "$out/summary.txt" <<'PY'
import json, pathlib, sys
root, out = map(pathlib.Path, sys.argv[1:3])
base = json.loads((root / 'mac-draw-hashes.json').read_text())
act = [json.loads(l) for l in (out / 'cpu-frame0/draws.jsonl').open()]
idx = lambda rows: {(r['draw'], t['target']): t['hash'] for r in rows for t in r.get('targets', [])}
a, b = idx(base), idx(act)
bad = [k for k in sorted(a.keys() | b.keys()) if a.get(k) != b.get(k)]
ids = [r['draw'] for r in base] == [r['draw'] for r in act]
print(f'targets mac {len(a)} linux {len(b)} mismatches {len(bad)} draw_ids_match {ids}')
sys.exit(1 if bad or not ids else 0)
PY
code=$?
echo "compare exit $code" >> "$out/summary.txt"
exit $code
