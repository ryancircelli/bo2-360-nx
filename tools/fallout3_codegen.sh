#!/usr/bin/env bash
# Fallout 3: codegen + post passes. Needs games/fallout3/work/default.image.bin (tools/xexdump). Run from anywhere.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REXGLUE="${REXGLUE:-$ROOT/out/host-mac/bin/rexglue}"
PY="${PYTHON:-python3}"
G="$ROOT/games/fallout3"; GEN="$G/generated/default"
cd "$G"
"$REXGLUE" codegen --ignore-stamp fallout3_manifest.toml > codegen.log 2>&1 || { echo "codegen failed, see $G/codegen.log" >&2; exit 1; }
tail -n 2 codegen.log
"$PY" "$ROOT/tools/read_before_write.py" --gen "$GEN" --strict
"$PY" "$ROOT/tools/codegen_gaps.py" --gen "$GEN" --prefix fallout3 --min 4 --image work/default.image.bin --toml "$G/gaps.toml" --text 0x82200000:0x82F9AE00
"$PY" "$ROOT/tools/setjmp_locals.py" --gen "$GEN" --fix
"$PY" "$ROOT/tools/direct_calls.py" --gen "$GEN"
"$PY" "$ROOT/tools/guest_barriers.py" --gen "$GEN"
if grep -rq 'REX_FATAL("Unresolved branch' "$GEN"; then echo "STILL UNRESOLVED BRANCHES" >&2; exit 1; fi
