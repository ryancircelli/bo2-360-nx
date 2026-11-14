#!/usr/bin/env bash
# Configures and builds bo2cp.nro (the campaign bring-up) for the Switch with devkitA64, libnx and Mesa's NVK, as
# games/bo2/build_switch.sh does for bo2zm, with link-time optimization (BO2_LTO=0 turns it off). Output:
# campaign/out/switch/bo2cp.nro. JOBS limits the parallel jobs (default 8: the generated code is big).
set -euo pipefail
CP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$CP/.."
SDK="${BO2_SDK_DIR:-$ROOT/sdk}"
OUT="$CP/out/switch"
LTO_LINK=""
if [ "${BO2_LTO:-1}" = "1" ]; then
  LTO_ARGS=("-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -flto=8 -ffat-lto-objects")
  LTO_LINK=" -flto=8"
else
  LTO_ARGS=("-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG")
fi
. "$ROOT/tools/toolchain.sh"
# The toolchain file passes its specs file to GCC unquoted: link from a copy in a folder without spaces.
SPECS="$BO2_SPECS_DIR/bo2cp/switch.specs"
cmake -S "$CP" -B "$OUT" -G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA_EXE" "-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out" \
  -DCMAKE_BUILD_TYPE=Release "${LTO_ARGS[@]}" \
  "-DBO2CP_HEAP_TRACK=$([ "${BO2_HEAP_TRACK:-}" = "1" ] && echo ON || echo OFF)" \
  "-DBO2CP_GUEST_MEMORY_PLAIN=$([ "${BO2_GUEST_MEMORY_PLAIN:-1}" = "1" ] && echo ON || echo OFF)" \
  "-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
  "-DREXSDK_DIR=$SDK" \
  "-DREXGLUE_SWITCH_NVK_SDK=$(cygpath -m "$MESA_SDK")" \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DCMAKE_EXE_LINKER_FLAGS=-specs=$SPECS -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax$LTO_LINK"
mkdir -p "$(dirname "$(cygpath -u "$SPECS")")"
cp "$OUT/switch.specs" "$(cygpath -u "$SPECS")"
# The generated code must come from campaign/codegen.sh (the direct calls, the setjmp fix and the barriers).
GEN="$CP/generated/default"
"${PYTHON:-python}" -I "$ROOT/tools/direct_calls.py" --check --gen "$GEN"
"${PYTHON:-python}" -I "$ROOT/tools/setjmp_locals.py" --verify --gen "$GEN"
"${PYTHON:-python}" -I "$ROOT/tools/guest_barriers.py" --check --gen "$GEN"
cmake --build "$OUT" --target bo2cp -j "${JOBS:-8}"
ls -la "$OUT/"*.nro
