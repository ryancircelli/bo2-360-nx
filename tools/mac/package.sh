#!/usr/bin/env bash
# Puts a macOS build together into a folder ready to play (tools/build_mac.sh runs it):
#
#   tools/mac/package.sh <build folder> <play folder> <shader folder> <data folder>
#
# <play folder> gets bo2zm and its libraries, MoltenVK (vulkan/, as the SDK stages it beside the build), the settings
# file (written once, then left alone so your changes stay), the shader library, links to the data folder's game/ and
# update/, and bo2zm.command to start it from Finder. Files are replaced, never rewritten in place: macOS keeps the
# code signature of a file by its inode, and a binary overwritten in place is killed at launch.
set -euo pipefail
OUT="$(cd "${1:?usage: tools/mac/package.sh <build> <play folder> <shaders> <data>}" && pwd)"
RUN="${2:?}"
SHADERS="$(cd "${3:?}" && pwd)"
DATA="$(cd "${4:?}" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

mkdir -p "$RUN"
put() {  # put <source> <destination>: a fresh file (new inode) every time
  rm -rf "$2"
  cp -R "$1" "$2"
}
put "$OUT/bo2zm" "$RUN/bo2zm"
for lib in librexruntime.dylib librexgpu-xenos.dylib libTracyClient.dylib; do
  src="$OUT/$lib"
  [ -f "$src" ] || src="$OUT/sdk-out/$lib"
  if [ -f "$src" ]; then
    put "$src" "$RUN/$lib"
  else
    echo "package: $lib not found in $OUT (skipped)" >&2
  fi
done
put "$OUT/vulkan" "$RUN/vulkan"
for f in bo2zm_shaders.spvpack bo2zm_shaders.spvidx; do
  put "$SHADERS/$f" "$RUN/$f"
done
[ -f "$RUN/bo2zm.toml" ] || cp "$ROOT/tools/mac/bo2zm.toml" "$RUN/bo2zm.toml"
for d in game update content; do
  if [ -d "$DATA/$d" ]; then
    ln -sfn "$DATA/$d" "$RUN/$d"
  fi
done
put "$ROOT/tools/mac/bo2zm.command" "$RUN/bo2zm.command"
chmod +x "$RUN/bo2zm" "$RUN/bo2zm.command"
echo "package: $RUN"
