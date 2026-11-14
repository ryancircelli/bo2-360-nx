#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REXGLUE="${REXGLUE:-$ROOT/out/host-linux/bin/rexglue}"
PY="${PYTHON:-python3}"
GEN="$ROOT/games/bo1/generated/default"
REPORT="${REPORT_DIR:-$ROOT/games/bo1}"

cd "$ROOT/games/bo1"

echo "== codegen"
"$REXGLUE" codegen --ignore-stamp bo1_manifest.toml > codegen.log 2>&1 || {
  tail -n 80 codegen.log >&2
  exit 1
}
tail -n 4 codegen.log

echo "== fix unresolved"
REXGLUE_BIN="$(dirname "$REXGLUE")" "$PY" "$ROOT/tools/codegen_fix_unresolved.py" \
    --project "$ROOT/games/bo1" --manifest bo1_manifest.toml \
    --image "$ROOT/games/bo1/work/default.image.bin"

echo "== registers read before written"
"$PY" "$ROOT/tools/read_before_write.py" --gen "$GEN" --strict

echo "== gap entries"
"$PY" "$ROOT/tools/codegen_gaps.py" --gen "$GEN" --prefix bo1 --min 4 \
    --image "$ROOT/games/bo1/work/default.image.bin" \
    --toml "$ROOT/games/bo1/gaps.toml" \
    --text 0x82000000:0x84070000 \
    --report "$REPORT/codegen_gaps.txt"

echo "== setjmp sites"
"$PY" "$ROOT/tools/setjmp_locals.py" --gen "$GEN" --fix

echo "== direct calls"
"$PY" "$ROOT/tools/direct_calls.py" --gen "$GEN"

echo "== guest memory barriers"
"$PY" "$ROOT/tools/guest_barriers.py" --gen "$GEN"

echo "== stats"
"$PY" "$ROOT/tools/codegen_stats.py" --gen "$GEN" || true

if grep -rq "REX_FATAL(\"Unresolved branch" "$GEN"; then
  echo "STILL UNRESOLVED BRANCHES:" >&2
  grep -rl "REX_FATAL(\"Unresolved branch" "$GEN" >&2
  exit 1
fi
echo "== no unresolved branches remain"
