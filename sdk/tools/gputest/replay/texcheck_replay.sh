#!/bin/bash
# Replays a trace with the texture staleness check and exits nonzero if any draw sampled a stale texture.
# For tools/gputest/regress.sh REPLAY=() entries.
#
#   texcheck_replay.sh <rexgpu-replay> <trace.xtr> <out_dir> [rexgpu-replay options, e.g. --plugin metal --race]
#
# Exit: 0 clean, 1 stale texture(s) (listed on stdout), 2 replay failed.
set -u
[ $# -ge 3 ] || { sed -n 2,7p "$0"; exit 2; }
replay="$1"; trace="$2"; out="$3"; shift 3
here="$(cd "$(dirname "$0")" && pwd)"
rm -rf "$out"; mkdir -p "$out"
"$replay" "$trace" -o "$out" --texcheck --no-color --no-depth "$@" > "$out/replay.log" 2>&1
st=$?
if [ ! -f "$out/texcheck.jsonl" ]; then
  echo "texcheck_replay: replay failed (exit $st), see $out/replay.log"
  exit 2
fi
python3 "$here/texcheck.py" "$out"
