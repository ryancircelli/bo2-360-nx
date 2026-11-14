#!/usr/bin/env bash
# Assembles the SD-card layout for the Switch build:
#   <out>/switch/bo2zm/bo2zm.nro
#   <out>/switch/bo2zm/bo2zm.toml
#   <out>/switch/bo2zm/bo2zm_shaders.spvpack   (native renderer shader library)
#   <out>/switch/bo2zm/bo2zm_shaders.spvidx    (its index, if made with bo2zm_pack --index)
#   <out>/switch/bo2zm/game/     (disc files + TU18 .xexp)
#   <out>/switch/bo2zm/update/   (TU18 package contents)
#   <out>/switch/bo2zm/user/0000000000000000/  (DLC map packs, from tools/install_dlc.py into $BO2_DATA/content)
# Game files are hard-linked from $BO2_DATA when it is on the same drive as
# <out> (no second copy); copy <out>/switch to the root of the SD card.
#
# usage: tools/stage_sd.sh [out_dir]     (default: sd/ in the data folder, tools/data_dir.sh)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$(bash "$ROOT/tools/data_dir.sh")"
OUT="${1:-$DATA/sd}"
DST="$OUT/switch/bo2zm"

mkdir -p "$DST"
cp "$ROOT/games/bo2/out/switch/bo2zm.nro" "$DST/"
cp "$ROOT/games/bo2/switch/bo2zm.toml" "$DST/"
# The native renderer's shader library (game-derived, built locally by tools/xenosrecomp).
[ -f "$DATA/shaders/bo2zm_shaders.spvpack" ] && cp "$DATA/shaders/bo2zm_shaders.spvpack" "$DST/bo2zm_shaders.spvpack"
# Its index, when it is at least as new as the pack; without it the first start makes one.
INDEX="$DATA/shaders/bo2zm_shaders.spvidx"
if [ -f "$INDEX" ] && [ ! "$INDEX" -ot "$DATA/shaders/bo2zm_shaders.spvpack" ]; then
  cp "$INDEX" "$DST/bo2zm_shaders.spvidx"
fi
for d in game update; do
  if [ ! -d "$DST/$d" ]; then
    cp -al "$DATA/$d" "$DST/$d" 2>/dev/null || cp -a "$DATA/$d" "$DST/$d"
  fi
done
# DLC map packs installed by tools/install_dlc.py: the runtime looks for them under user/ (marketplace content is
# shared, xuid 0000000000000000). Merged into an existing user/ folder; saves are never touched.
if [ -d "$DATA/content/0000000000000000" ]; then
  mkdir -p "$DST/user"
  cp -al "$DATA/content/0000000000000000" "$DST/user/" 2>/dev/null || cp -a "$DATA/content/0000000000000000" "$DST/user/"
fi
du -sh "$DST" "$DST/game" "$DST/update" "$DST/user" 2>/dev/null
ls -la "$DST"
