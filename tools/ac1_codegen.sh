#!/usr/bin/env bash
# Regenerate the ac1 recompiled code with the fixed worktree rexglue and re-run
# every post-codegen pass, in the order the first attempt used them.
#
#   tools/ac1_codegen.sh          (from the worktree root)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REXGLUE="${REXGLUE:-$HOME/dev/bo2-360-nx/out/host-mac/bin/rexglue}"
PY="/tmp/bo2/newgame-ac1/venv/bin/python"
GEN="$ROOT/games/ac1/generated/default"

cd "$ROOT/games/ac1"

echo "== fix unresolved"
REXGLUE_BIN="$(dirname "$REXGLUE")" "$PY" "$ROOT/tools/codegen_fix_unresolved.py" \
    --project "$ROOT/games/ac1" --manifest ac1_manifest.toml \
    --image "$ROOT/games/ac1/work/default.image.bin"

echo "== codegen (fixed emitter)"
"$REXGLUE" codegen --ignore-stamp ac1_manifest.toml > codegen.log 2>&1 || {
  echo "codegen failed, see games/ac1/codegen.log" >&2; exit 1
}
tail -n 2 codegen.log

echo "== registers read before written"
"$PY" "$ROOT/tools/read_before_write.py" --gen "$GEN" --strict

echo "== gap entries (min 4: pulls in the tiny thunk/stub gaps below the default 8)"
"$PY" "$ROOT/tools/codegen_gaps.py" --gen "$GEN" --prefix ac1 --min 4 \
    --image "$ROOT/games/ac1/work/default.image.bin" \
    --toml "$ROOT/games/ac1/gaps.toml" \
    --text 0x82130000:0x82B91200 \
    --report /tmp/bo2/newgame-ac1/codegen_gaps.txt

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
