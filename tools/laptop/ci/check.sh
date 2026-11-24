#!/bin/bash
# Laptop CI: check one bo2-360-nx branch. usage: check.sh BRANCH [--bless]
# Parts: build (SDK, incremental), unit (ctest -L unit), replay (refxenos CPU replay of saved traces vs goldens),
# codegen (SKIP: needs game images, not on laptop), shaders (covered by unit graphics tests; no separate sweep tool in main).
# Results: ~/bo2-worker/ci/results/<sha>/{summary.txt,*.log}. Exit 0 only when no part FAILs.
set -u
CI=$HOME/bo2-worker/ci
branch=${1:?branch}; bless=${2:-}
exec 9>"$CI/lock"; flock 9          # one check at a time: 8 cores / 7 GB RAM
cd "$CI/repo" || exit 2
git fetch -q origin "$branch" 2>&1 || { echo "FAIL fetch: no branch $branch on origin"; exit 2; }
git checkout -q --detach FETCH_HEAD || exit 2
sha=$(git rev-parse --short=10 HEAD)
out=$CI/results/$sha; mkdir -p "$out"; : > "$out/summary.txt"
echo "$branch $sha" > "$out/branch.txt"
rec() { printf '%-8s %-5s %s\n' "$1" "$2" "$3" | tee -a "$out/summary.txt"; }
t0=$(date +%s)
part_time() { echo "$(( $(date +%s) - $1 ))s"; }
rc=0

# build: re-run cmake (sdk/ changes may add files), then incremental ninja
t=$(date +%s)
if cmake -S sdk -B "$CI/build" >"$out/build.log" 2>&1 && ninja -C "$CI/build" -j2 >>"$out/build.log" 2>&1; then
  rec build PASS "$(part_time $t)"
else rec build FAIL "$(grep -m1 -E 'error:|FAILED' "$out/build.log") (see $out/build.log)"; rc=1; fi

# unit
if [ $rc = 0 ]; then
  t=$(date +%s)
  (cd "$CI/build" && ctest -L unit -j2 --output-on-failure) >"$out/unit.log" 2>&1
  r=$(grep -E "tests passed" "$out/unit.log" | tail -1)
  if grep -q "100% tests passed" "$out/unit.log"; then rec unit PASS "$r, $(part_time $t)"
  else rec unit FAIL "${r:-no result} ($(grep -E '\*\*\*Failed|Failed' "$out/unit.log" | head -3 | tr '\n' ';'))"; rc=1; fi
else rec unit SKIP "build failed"; fi

# replay: pinned refxenos build (sdkfork refxenos-1reverse) over each saved trace; hashes vs goldens
ref=$CI/ref-build/refxenos
bad=0; n=0; nog=""
t=$(date +%s)
for x in "$CI"/traces/*.xtr; do
  name=$(basename "$x" .xtr); g=$CI/goldens/$name.json; d=$out/replay-$name; n=$((n+1))
  "$ref" replay "$x" -o "$d" --rop edram --frame 0 >"$d.log" 2>&1
  [ -f "$d/draws.jsonl" ] || { rec replay FAIL "$name: no draws.jsonl, see $d.log"; bad=1; continue; }
  # slim golden = {draw: {target: hash}}; accepts the Mac baseline format too
  python3 - "$d/draws.jsonl" "$g" "$bless" > "$d.cmp" <<'PY'
import json, sys, os
act, gold, bless = sys.argv[1:4]
rows = [json.loads(l) for l in open(act)]
cur = {str(r['draw']): {t['target']: t['hash'] for t in r.get('targets', [])} for r in rows}
if not os.path.exists(gold):
    if bless == '--bless': json.dump(cur, open(gold, 'w')); print('BLESSED', len(cur)); sys.exit(0)
    print('NOGOLDEN'); sys.exit(3)
g = json.load(open(gold))
if isinstance(g, list): g = {str(r['draw']): {t['target']: t['hash'] for t in r.get('targets', [])} for r in g}
if bless == '--bless': json.dump(cur, open(gold, 'w')); print('RE-BLESSED', len(cur)); sys.exit(0)
bad = [k for k in sorted(g.keys() | cur.keys(), key=int) if g.get(k) != cur.get(k)]
print(f'draws {len(cur)} mismatches {len(bad)}' + (f' first draw {bad[0]}' if bad else ''))
sys.exit(1 if bad else 0)
PY
  c=$?; [ $c = 0 ] || [ $c = 3 ] || bad=1
  [ $c = 3 ] && nog="$nog $name"
  echo "  $name: $(cat "$d.cmp")"
  [ -d "$d" ] && rm -rf "$d"   # keep logs, drop per-draw npy/png
done
if [ $bad = 1 ]; then rec replay FAIL "see $out/replay-*.cmp"; rc=1
else rec replay PASS "$n traces, $(part_time $t)${nog:+, no golden (run with --bless):$nog}"; fi

rec codegen SKIP "needs decrypted game images, not stored on laptop"
rec shaders SKIP "no standalone sweep tool in bo2-360-nx main; unit graphics tests run in unit"
echo "TOTAL $(part_time $t0) -> $([ $rc = 0 ] && echo PASS || echo FAIL) $branch $sha" | tee -a "$out/summary.txt"
ln -sfn "$out" "$CI/results/latest"
exit $rc
