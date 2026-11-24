#!/bin/bash
# Laptop CI: check one bo2-360-nx branch. usage: check.sh BRANCH [--bless]
# Parts: build (SDK, incremental), unit (ctest, all registered tests), replay (refxenos CPU replay of saved traces vs goldens),
# codegen (rexglue codegen on the TU18 default_mp image in $CI/work/load_tu18, then the strict codegen self-checks), shaders
# (every shader microcode in the traces through rexgpu-shadersweep, the SDK SPIR-V translator).
# --bless seeds missing goldens only; --rebless overwrites all.
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
  (cd "$CI/build" && ctest -j2 --output-on-failure) >"$out/unit.log" 2>&1
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
  "$ref" replay "$x" -o "$d" --rop edram --frame 0 --dump-shaders >"$d.log" 2>&1
  mkdir -p "$out/shaders"; cp -n "$d"/shaders/*.bin "$out/shaders/" 2>/dev/null
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
if bless == '--rebless': json.dump(cur, open(gold, 'w')); print('RE-BLESSED', len(cur)); sys.exit(0)
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

# codegen: tools/codegen.sh = rexglue codegen + read_before_write --strict + setjmp/direct-call/barrier fix-ups + stats.
# The image is the owner's own TU18 dump, copied once to $CI/work/load_tu18 (never in the repo); output goes to $CI/gen.
sdkbin=$CI/repo/sdk/out/linux-amd64
if [ -f "$CI/work/load_tu18/default_mp.xex" ] && [ -x "$sdkbin/rexglue" ]; then
  t=$(date +%s); mkdir -p "$CI/gen"; ln -sfn "$CI/work" work; ln -sfn "$CI/gen" app/generated
  if REXGLUE_BIN_DIR=$sdkbin PYTHON=python3 BO2_GENERATED_LINK_OK=1 nice bash tools/codegen.sh >"$out/codegen.log" 2>&1; then
    rec codegen PASS "$(grep -m1 -o 'TOTAL [0-9]* functions.*' "$out/codegen.log"), $(part_time $t)"
  else rec codegen FAIL "$(tail -2 "$out/codegen.log" | tr '\n' ' ')(see $out/codegen.log; app/codegen.log)"; rc=1; fi
else rec codegen SKIP "no TU18 image in $CI/work/load_tu18 or no rexglue build"; fi

# shaders: translate every ps_/vs_ microcode dumped by the replays above
if [ -x "$sdkbin/rexgpu-shadersweep" ] && ls "$out"/shaders/*.bin >/dev/null 2>&1; then
  "$sdkbin/rexgpu-shadersweep" "$out"/shaders/*.bin >"$out/shaders.log" 2>&1
  if [ $? = 0 ]; then rec shaders PASS "$(tail -1 "$out/shaders.log")"
  else rec shaders FAIL "$(grep -m1 FAIL "$out/shaders.log") ($(tail -1 "$out/shaders.log"), see $out/shaders.log)"; rc=1; fi
else rec shaders SKIP "no rexgpu-shadersweep build or no shader dumps"; fi
echo "TOTAL $(part_time $t0) -> $([ $rc = 0 ] && echo PASS || echo FAIL) $branch $sha" | tee -a "$out/summary.txt"
ln -sfn "$out" "$CI/results/latest"
exit $rc
