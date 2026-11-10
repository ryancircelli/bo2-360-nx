#!/usr/bin/env bash
# Builds the ReXGlue host tools (rexglue, the code generator, and xexdump) natively on Linux, x86_64 or ARM64.
# The Linux counterpart of tools/build_host.sh (Windows) and the host step of tools/build_mac.sh.
#
#   bash tools/linux/build_host.sh
#
# Needs: clang 19 or newer (Ubuntu 24.04: clang-20 lld-20), cmake, ninja, python3, git.
# Output: out/host-linux/ (the SDK's libraries and the rexglue binary in sdk/out/linux-<arch>/, xexdump in
# out/host-linux/switch_bo2/). rexglue loads librexruntime.so from its own folder, so for codegen:
#
#   export REXGLUE_BIN_DIR=$PWD/sdk/out/linux-arm64 LD_LIBRARY_PATH=$PWD/sdk/out/linux-arm64 PYTHON=python3
#   bash tools/codegen.sh
#
# Environment: CLANG_SUFFIX (e.g. -20), JOBS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="$ROOT/sdk"
HOST="$ROOT/out/host-linux"
if [ -z "${CLANG_SUFFIX+set}" ]; then
  for s in "" -22 -21 -20 -19; do
    v="$( { "clang$s" -dumpversion 2> /dev/null || true; } | cut -d. -f1)"
    if [ -n "$v" ] && [ "$v" -ge 19 ]; then CLANG_SUFFIX="$s"; break; fi
  done
fi
: "${CLANG_SUFFIX:?clang 19 or newer is needed (Ubuntu 24.04: apt install clang-20 lld-20)}"
JOBS="${JOBS:-$(nproc)}"

[ -d "$SDK/thirdparty/cli11/include" ] || python3 "$ROOT/tools/fetch_thirdparty.py"
# The SDK's version script expects a .git in each third-party folder.
for d in "$SDK"/thirdparty/*/; do [ -e "$d.git" ] || mkdir -p "$d.git"; done

# No -march: the SDK presets use x86-64-v2 on x86_64 only. GIT_EXECUTABLE points nowhere so the version check uses its
# floor (0.10).
cmake -S "$SDK" -B "$HOST" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_COMPILER=clang$CLANG_SUFFIX" "-DCMAKE_CXX_COMPILER=clang++$CLANG_SUFFIX" \
  -DGIT_EXECUTABLE=/nonexistent/git "-DCMAKE_PROJECT_ReXGlue_INCLUDE=$ROOT/cmake/host_tools_inject.cmake"
cmake --build "$HOST" --target rexglue xexdump -j "$JOBS"
echo "rexglue: $(ls "$SDK"/out/linux-*/rexglue)"
echo "xexdump: $HOST/switch_bo2/xexdump"
