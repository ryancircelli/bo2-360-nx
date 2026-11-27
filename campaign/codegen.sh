#!/usr/bin/env bash
# Runs the code generator on the campaign bring-up (bo2cp_manifest.toml -> campaign/generated/default) and the fix-up
# steps its build depends on, as tools/codegen.sh does for the Zombies program:
#   1. rexglue codegen (campaign/codegen.log);
#   2. tools/codegen_gaps.py: code the analyzer missed (gaps.toml), which takes effect on the next run: run this twice
#      after a change that adds functions;
#   3. tools/read_before_write.py: registers read before they are written (campaign/codegen_read_before_write.log);
#      a new entry needs a look, and share_registers in overrides.toml if it is a piece of a split function;
#   4. tools/setjmp_locals.py --fix, tools/direct_calls.py, tools/guest_barriers.py.
# Tail-branch targets in no function are declared by the code generator itself (sdk/src/codegen/analyze.cpp).
# REXGLUE_BIN: the folder with rexglue.exe (default sdk/out/win-amd64 of this checkout).
set -euo pipefail
CP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$CP/.."
BIN="${REXGLUE_BIN:-$ROOT/sdk/out/win-amd64}"
export PATH="$BIN:$PATH"
PY="${PYTHON:-python}"
GEN=campaign/generated/default
(cd "$CP" && rexglue.exe codegen --ignore-stamp bo2cp_manifest.toml > codegen.log 2>&1) || {
  echo "codegen failed, see campaign/codegen.log" >&2
  exit 1
}
tail -n 2 "$CP/codegen.log"
cd "$ROOT"
"$PY" tools/codegen_gaps.py --gen "$GEN" --prefix bo2cp --image work/dump_tu18/default.image.bin \
  --text 82100000:8270D8B4 --image-size 38731776 --toml campaign/gaps.toml --report campaign/codegen_gaps.txt --overrides campaign/overrides.toml
"$PY" -I tools/read_before_write.py --gen "$GEN" > campaign/codegen_read_before_write.log
head -n 1 campaign/codegen_read_before_write.log
"$PY" -I tools/setjmp_locals.py --gen "$GEN" --fix
"$PY" -I tools/direct_calls.py --gen "$GEN"
"$PY" -I tools/guest_barriers.py --gen "$GEN"
