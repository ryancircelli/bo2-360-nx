#!/usr/bin/env bash
# Regenerates a small game's code by hand (games/<game>/generated/default) and refreshes its gaps.toml: rexglue
# codegen, tools/codegen_fix_unresolved.py, tools/codegen_gaps.py, then the fix-up passes of the game's app.toml
# [source] codegen_passes (the ones tools/mac/build_app.py runs), then a check that no unresolved branch is left.
#
#   tools/game_codegen.sh <game> <.text START:END>
#
# The .text ranges used so far: bo1 0x82000000:0x84070000, nfsmw 0x82000000:0x82CD0000, ac1 0x82130000:0x82B91200.
# Needs games/<game>/work/default.image.bin. REXGLUE (default out/host-<os>/bin/rexglue), PYTHON and REPORT_DIR
# (default games/<game>) override.
set -euo pipefail
[ $# -eq 2 ] || { sed -n '2,10p' "$0" >&2; exit 2; }
GAME="$1" TEXT="$2"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OS="$([ "$(uname)" = Darwin ] && echo mac || echo linux)"
REXGLUE="${REXGLUE:-$ROOT/out/host-$OS/bin/rexglue}"
PY="${PYTHON:-python3}"
DIR="$ROOT/games/$GAME"
GEN="$DIR/generated/default"
IMAGE="$DIR/work/default.image.bin"

cd "$DIR"
echo "== codegen"
"$REXGLUE" codegen --ignore-stamp "${GAME}_manifest.toml" > codegen.log 2>&1 || { tail -n 80 codegen.log >&2; exit 1; }
tail -n 4 codegen.log

echo "== fix unresolved"
REXGLUE_BIN="$(dirname "$REXGLUE")" "$PY" "$ROOT/tools/codegen_fix_unresolved.py" \
    --project "$DIR" --manifest "${GAME}_manifest.toml" --image "$IMAGE"

echo "== gap entries (min 4: pulls in the tiny thunk/stub gaps below the default 8)"
"$PY" "$ROOT/tools/codegen_gaps.py" --gen "$GEN" --prefix "$GAME" --min 4 --image "$IMAGE" \
    --toml "$DIR/gaps.toml" --text "$TEXT" --report "${REPORT_DIR:-$DIR}/codegen_gaps.txt"

# Same arguments as tools/mac/build_app.py codegen().
for p in $("$PY" -I -c 'import sys,tomllib; print(" ".join(tomllib.load(open(sys.argv[1],"rb")).get("source",{}).get("codegen_passes",[])))' app.toml); do
  echo "== $p"
  case "$p" in
    read_before_write) "$PY" "$ROOT/tools/$p.py" --gen "$GEN" --strict ;;
    setjmp_locals) "$PY" "$ROOT/tools/$p.py" --gen "$GEN" --fix ;;
    *) "$PY" "$ROOT/tools/$p.py" --gen "$GEN" ;;
  esac
done

echo "== stats"
"$PY" "$ROOT/tools/codegen_stats.py" --gen "$GEN" || true

if grep -rq "REX_FATAL(\"Unresolved branch" "$GEN"; then
  echo "STILL UNRESOLVED BRANCHES:" >&2
  grep -rl "REX_FATAL(\"Unresolved branch" "$GEN" >&2
  exit 1
fi
echo "== no unresolved branches remain"
