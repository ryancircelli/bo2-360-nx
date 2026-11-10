#!/usr/bin/env bash
# Runs the code generator on app/ and the fix-up steps the build depends on, in order. Use this instead of calling
# rexglue codegen by hand: a bare codegen silently loses the setjmp fix and the direct calls.
#
#   tools/codegen.sh
#
# 1. rexglue codegen app/bo2zm_manifest.toml (the log goes to app/codegen.log). It writes app/generated/default_mp.
# 2. tools/read_before_write.py --strict: registers read before they are written (split functions that need share_registers).
#    Stops on a finding that is not in its KNOWN list.
# 3. tools/setjmp_locals.py --fix: register locals restored after the game's longjmp (Com_Error recovery).
# 4. tools/direct_calls.py: direct calls to the game functions that have no hook.
#    tools/guest_barriers.py: compiler barriers after labels and ordering instructions (app/src/bo2zm_guest_memory.h)
#    tools/localize.py: copies of the hot leaf functions in app/locals.txt with their ctx registers as locals
#    (app/src/bo2zm_native_audio.cpp checks them against the originals before they run alone).
# 5. tools/codegen_stats.py: what the generated code looks like now.
# New unresolved tail-branch targets (codegen errors) are a job for tools/codegen_fix_unresolved.py first.
#
# In a worktree, app/generated must be this checkout's own copy (tools/bootstrap_worktree.sh --own-generated), never
# the junction to the main checkout's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# BO2_GENERATED_LINK_OK=1 (tools/build_mac.sh): app/generated is a link to a folder outside every checkout (the data
# folder), which is what keeps generated code out of the repository; the check above is for worktree junctions.
if [ -L "$ROOT/app/generated" ] && [ "${BO2_GENERATED_LINK_OK:-0}" != 1 ]; then
  echo "app/generated is a link to another checkout's generated code: run tools/bootstrap_worktree.sh <tag> --own-generated" >&2
  echo "(after removing the link), or run this in the checkout that owns it" >&2
  exit 1
fi
SDK_BIN="${REXGLUE_BIN_DIR:-$ROOT/sdk/out/win-amd64}"
export PATH="$SDK_BIN:$PATH"
PY="${PYTHON:-python}"
# Linux distributions often have only python3.
if [ -z "${PYTHON:-}" ] && ! command -v python > /dev/null 2>&1; then PY=python3; fi
# rexglue.exe on Windows, rexglue elsewhere (tools/build_mac.sh sets REXGLUE_BIN_DIR to its host build).
REXGLUE=rexglue.exe
command -v "$REXGLUE" > /dev/null 2>&1 || REXGLUE=rexglue

echo "== codegen"
(cd "$ROOT/app" && "$REXGLUE" codegen --ignore-stamp bo2zm_manifest.toml > codegen.log 2>&1) || {
  echo "codegen failed, see app/codegen.log" >&2
  exit 1
}
tail -n 2 "$ROOT/app/codegen.log"
echo "== registers read before written"
"$PY" "$ROOT/tools/read_before_write.py" --strict > "$ROOT/app/codegen_read_before_write.log"
echo "== setjmp sites"
"$PY" "$ROOT/tools/setjmp_locals.py" --fix
echo "== direct calls"
"$PY" "$ROOT/tools/direct_calls.py"
echo "== local copies of hot leaf functions (app/locals.txt)"
"$PY" "$ROOT/tools/localize.py"
echo "== guest memory barriers"
"$PY" "$ROOT/tools/guest_barriers.py"
echo "== stats"
"$PY" "$ROOT/tools/codegen_stats.py"
