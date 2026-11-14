#!/usr/bin/env bash
# Stages only the files that changed since the last delivery to the SD card.
#
#   $BO2_DATA/sd-update/switch/bo2zm/...   <- copy this "switch" folder onto the
#                                             SD root and merge/overwrite
#
# What was delivered is recorded in $BO2_DATA/sd/.delivered.sha256 (created
# from the full staging folder of tools/stage_sd.sh the first time). Game files
# (game/, update/) never change between builds and are not compared.
#
# usage: tools/stage_update.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$(bash "$ROOT/tools/data_dir.sh")"
FULL="$DATA/sd/switch/bo2zm"
OUT="$DATA/sd-update/switch/bo2zm"
MANIFEST="$DATA/sd/.delivered.sha256"

declare -A SOURCES=(
  [bo2zm.nro]="$ROOT/games/bo2/out/switch/bo2zm.nro"
  [bo2zm.toml]="$ROOT/games/bo2/switch/bo2zm.toml"
  # The native renderer's shader library: game-derived, built locally (tools/xenosrecomp), never in the repo.
  [bo2zm_shaders.spvpack]="$DATA/shaders/bo2zm_shaders.spvpack"
)

hash_of() { sha256sum "$1" | cut -d' ' -f1; }

if [ ! -f "$MANIFEST" ]; then
  # First run: what tools/stage_sd.sh staged is what the SD card has.
  for name in "${!SOURCES[@]}"; do
    [ -f "$FULL/$name" ] && echo "$(hash_of "$FULL/$name")  $name" >> "$MANIFEST"
  done
fi

rm -rf "$DATA/sd-update"
mkdir -p "$OUT"
changed=0
for name in "${!SOURCES[@]}"; do
  src="${SOURCES[$name]}"
  new="$(hash_of "$src")"
  old="$(grep "  $name\$" "$MANIFEST" | cut -d' ' -f1 || true)"
  if [ "$new" != "$old" ]; then
    cp "$src" "$OUT/$name"
    cp "$src" "$FULL/$name"   # keep the full mirror current
    grep -v "  $name\$" "$MANIFEST" > "$MANIFEST.tmp" || true
    echo "$new  $name" >> "$MANIFEST.tmp"
    mv "$MANIFEST.tmp" "$MANIFEST"
    echo "changed: switch/bo2zm/$name"
    changed=$((changed + 1))
  fi
done
if [ "$changed" -eq 0 ]; then
  echo "nothing changed since the last delivery"
  rm -rf "$DATA/sd-update"
else
  echo "copy $(cygpath -w "$DATA/sd-update")\\switch onto the SD card root (overwrite),"
fi
