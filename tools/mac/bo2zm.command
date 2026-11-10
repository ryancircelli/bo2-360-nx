#!/usr/bin/env bash
# Starts Black Ops II Zombies on macOS (double-click in Finder, or run from a terminal with extra --name=value
# options). Settings are in bo2zm.toml next to this file.
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1

missing=""
[ -f game/default_mp.xex ] || missing="$missing game/default_mp.xex"
[ -f game/default_mp.xexp ] || missing="$missing game/default_mp.xexp"
[ -d update ] || missing="$missing update/"
[ -f bo2zm_shaders.spvpack ] || missing="$missing bo2zm_shaders.spvpack"
if [ -n "$missing" ]; then
  echo "bo2zm: missing next to this script:$missing (run tools/build_mac.sh)" >&2
  exit 1
fi

# MoltenVK: no fast math. With it Metal may turn clamp(rsqrt(0)) * 0 into NaN (seen in the GTA V recompilation's
# Mac port), and the game's shaders were written for the Xbox 360's IEEE results.
export MVK_CONFIG_FAST_MATH_ENABLED="${MVK_CONFIG_FAST_MATH_ENABLED:-0}"

renderer="--bo2zm_renderer=native"
own_user=""
for a in "$@"; do
  case "$a" in
    --bo2zm_renderer=*) renderer="" ;;
    --user_data_root=*) own_user=1 ;;
  esac
done
mkdir -p user
if [ -z "$own_user" ]; then
  set -- "--user_data_root=$here/user" "$@"
fi
exec ./bo2zm $renderer "$@"
