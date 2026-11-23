#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
THIRDPARTY="${THIRDPARTY_ROOT:-$ROOT/sdk/thirdparty}"

shopt -s nullglob
for patch_file in "$ROOT"/patches/*.patch; do
  name="$(basename "$patch_file")"
  case "$name" in
    mesa-*) continue ;;
    sdl3-*) library=sdl3 ;;
    *) echo "$name: no third-party target; name it <library>-<change>.patch" >&2; exit 1 ;;
  esac
  target="$THIRDPARTY/$library"
  if [ ! -d "$target" ]; then
    echo "$target not found; run tools/fetch_thirdparty.py first" >&2
    exit 1
  fi
  if patch -s -N -p1 -d "$target" --dry-run < "$patch_file"; then
    patch -s -N -p1 -d "$target" < "$patch_file"
    echo "applied $name to $library"
  elif patch -s -N -R -p1 -d "$target" --dry-run < "$patch_file"; then
    echo "$name already applied to $library"
  else
    echo "$name does not apply cleanly to $target" >&2
    exit 1
  fi
done
