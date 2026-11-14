#!/usr/bin/env bash
# Lays out the Linux ARM64 runtime package from a finished build (games/bo2/build_linux.sh runs it):
#
#   tools/linux/package.sh <build folder>
#
#   <build folder>/package/bo2zm-linux-arm64/
#     bo2zm                 the game (recompiled code, runtime and native renderer in one executable)
#     librexruntime.so      the SDK's runtime (kernel, file system, audio, input)
#     librexgpu-xenos.so    the emulated Xenos GPU, used only with --bo2zm_renderer=xenos --gpu_plugin=xenos
#     bo2zm.sh              the launcher: native renderer, checks that the game files are in place
#     bo2zm.toml            the settings, with the defaults written out
#     README.txt            what goes next to it
#   <build folder>/package/bo2zm-linux-arm64.tar.gz
#
# The binaries carry code recompiled from the player's own copy of the game, so the package is for that player's
# machines only: never upload or share it. Game files, the update and the shader library are not in it; README.txt
# says where they go.
set -euo pipefail
OUT="$(cd "${1:?usage: tools/linux/package.sh <build folder>}" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="$OUT/package/bo2zm-linux-arm64"
STRIP="$(command -v "llvm-strip${CLANG_SUFFIX:-}" || command -v llvm-strip || true)"

rm -rf "$PKG"
mkdir -p "$PKG"
cp "$OUT/bo2zm" "$PKG/"
for lib in librexruntime.so librexgpu-xenos.so; do
  src="$OUT/$lib"
  [ -f "$src" ] || src="$OUT/sdk-out/$lib"
  if [ -f "$src" ]; then
    cp "$src" "$PKG/"
  else
    echo "package: $lib not found in $OUT (skipped)" >&2
  fi
done
if [ -n "$STRIP" ]; then
  "$STRIP" --strip-unneeded "$PKG/bo2zm" "$PKG"/*.so
fi
cp "$ROOT/tools/linux/bo2zm.sh" "$PKG/bo2zm.sh"
cp "$ROOT/tools/linux/bo2zm.toml" "$PKG/bo2zm.toml"
cp "$ROOT/tools/linux/README-package.txt" "$PKG/README.txt"
chmod +x "$PKG/bo2zm" "$PKG/bo2zm.sh"

tar -C "$OUT/package" -czf "$OUT/package/bo2zm-linux-arm64.tar.gz" bo2zm-linux-arm64
echo "package: $PKG"
ls -la "$PKG"
