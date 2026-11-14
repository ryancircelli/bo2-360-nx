#!/usr/bin/env bash
# Configures and builds bo2zm.nro for the Switch with devkitA64, libnx and Mesa's NVK Vulkan driver.
# Needs the generated C++ (tools/codegen.sh), devkitPro and the Mesa SDK that tools/build_mesa.sh builds; where each
# tool is installed comes from tools/toolchain.sh (BO2_DEVKITPRO, MESA_SDK, BO2_NINJA, ...). The toolchain file is in
# tools/switch/cmake. The output is games/bo2/out/switch/bo2zm.nro.
# Options, all from the environment: BO2_LTO, BO2_PGO and BO2_PGO_DIR, BO2_DEVLINK, BO2_GUEST_MEMORY_PLAIN, BO2_BUILD_TAG,
# BO2_HEAP_TRACK, BO2_MEMCPY_CALLERS, BO2_DEBUG_LINES, JOBS and BO2ZM_BUILD_ID (the build's name in its logs; the date
# and time by default), each explained where it is read below.
set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd -P)"
ROOT="$APP/../.."
BO2_DATA="$(bash "$ROOT/tools/data_dir.sh")"
SDK="${BO2_SDK_DIR:-$ROOT/sdk}"
# Several checkouts at once (git worktrees prepared by tools/bootstrap_worktree.sh): BO2_BUILD_TAG gives this checkout
# its own build folder, its own copy of the SDK's libraries and its own specs file. Without it: games/bo2/out/switch.
OUT="$APP/out/switch${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}"
SDK_OUT_ARG=()
if [ -n "${BO2_BUILD_TAG:-}" ]; then
  SDK_OUT_ARG=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
fi
# Link-time optimization, on by default (BO2_LTO=0 turns it off): GCC -flto, with fat objects so the toolchain's plain
# ar still indexes the static libraries. The game's ~30,000 generated functions sit in ~140 files, so calls between
# files can only be inlined this way. On the console (Town benchmark route, very low preset, FPS cap 60, CPU 1785 /
# GPU 768 / memory 1600 MHz, CPU-bound) it took the average from 37.7 to 41.4 FPS and the median frame from 26.2 to
# 24.0 ms, halved the frames over 33.3 ms (5.4 to 2.6 %), and the job workers used about 13 % less CPU per frame. The
# Release flags are passed either way, so switching it in one build folder never keeps the other's from CMake's cache.
# Profile-guided optimization (GCC) of the generated code (games/bo2/CMakeLists.txt, BO2ZM_PGO). BO2_PGO=generate: a
# training build (BO2_BUILD_TAG=pgo by default, its own folder) whose counters are written to sdmc:/switch/bo2zm/pgo
# when the benchmark route ends (bo2zm_benchmark.cpp; reset when it starts). Copy that folder to $BO2_DATA/pgo, then
# BO2_PGO=use builds with the profile.
if [ "${BO2_PGO:-}" = "generate" ] && [ -z "${BO2_BUILD_TAG:-}" ]; then
  export BO2_BUILD_TAG=pgo
  OUT="$APP/out/switch-pgo"
  SDK_OUT_ARG=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
fi
# BO2_HEAP_TRACK=1: a test build that logs the C heap's live bytes by caller every minute (games/bo2/CMakeLists.txt).
HEAP_ARGS=("-DBO2ZM_HEAP_TRACK=OFF")
if [ "${BO2_HEAP_TRACK:-}" = "1" ]; then
  HEAP_ARGS=("-DBO2ZM_HEAP_TRACK=ON")
fi
# BO2_MEMCPY_CALLERS=1: a test build that logs the ring thread's memcpy calls by call site every 10 s (games/bo2/CMakeLists.txt).
HEAP_ARGS+=("-DBO2ZM_MEMCPY_CALLERS=$([ "${BO2_MEMCPY_CALLERS:-}" = "1" ] && echo ON || echo OFF)")
# BO2_DEBUG_LINES=1: a profile build with line tables for the native renderer (games/bo2/CMakeLists.txt, BO2ZM_DEBUG_LINES).
DEBUG_ARGS=("-DBO2ZM_DEBUG_LINES=$([ "${BO2_DEBUG_LINES:-}" = "1" ] && echo ON || echo OFF)")
# BO2_DEVLINK=0: a build without the development consoles' PC link (games/bo2/CMakeLists.txt, BO2ZM_DEVLINK).
DEVLINK_ARGS=("-DBO2ZM_DEVLINK=$([ "${BO2_DEVLINK:-1}" = "0" ] && echo OFF || echo ON)")
PGO_ARGS=()
case "${BO2_PGO:-}" in
  generate) PGO_ARGS=("-DBO2ZM_PGO=generate") ;;
  use) PGO_ARGS=("-DBO2ZM_PGO=use" "-DBO2ZM_PGO_DIR=$(cygpath -m "${BO2_PGO_DIR:-${BO2_DATA}/pgo}")") ;;
  "") PGO_ARGS=("-DBO2ZM_PGO=") ;;
  *) echo "BO2_PGO must be generate or use" >&2; exit 1 ;;
esac
# Plain guest loads and stores in the generated code (core/bo2zm_guest_memory.h), on by default for the Switch
# (about +5.8 % on the console, measured together with the code generation changes that came with it).
# BO2_GUEST_MEMORY_PLAIN=0 builds the volatile form.
GUEST_ARGS=("-DBO2ZM_GUEST_MEMORY_PLAIN=$([ "${BO2_GUEST_MEMORY_PLAIN:-1}" = "1" ] && echo ON || echo OFF)")
LTO_LINK=""
if [ "${BO2_LTO:-1}" = "1" ]; then
  LTO_ARGS=("-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -flto=8 -ffat-lto-objects")
  LTO_LINK=" -flto=8"
else
  LTO_ARGS=("-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG")
fi
# hbmenu icon (256x256 JPEG). It carries the game's logo, so it lives with the game data, outside the repo.
# Missing: an empty value, and the NRO gets libnx's default icon.
NRO_ICON="${BO2ZM_NRO_ICON:-${BO2_DATA}/art/icon_switch.jpg}"
if [ -f "$NRO_ICON" ]; then NRO_ICON="$(cygpath -m "$NRO_ICON")"; else NRO_ICON=""; fi
# CMake, Ninja, devkitPro (DEVKITPRO) and the Mesa SDK (MESA_SDK), from tools/toolchain.sh. The cross toolchain file
# limits program lookups to the target sysroot, so CMake would not find Ninja on PATH: it is named explicitly.
. "$ROOT/tools/toolchain.sh"
SPECS="$BO2_SPECS_DIR${BO2_BUILD_TAG:+/$BO2_BUILD_TAG}/switch.specs"
cmake -S "$APP" -B "$OUT" -G Ninja "-DCMAKE_MAKE_PROGRAM=$NINJA_EXE" "${SDK_OUT_ARG[@]}" \
  -DCMAKE_BUILD_TYPE=Release "${LTO_ARGS[@]}" "${PGO_ARGS[@]}" "${HEAP_ARGS[@]}" "${GUEST_ARGS[@]}" "${DEBUG_ARGS[@]}" \
  "${DEVLINK_ARGS[@]}" \
  "-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
  "-DREXSDK_DIR=$SDK" \
  "-DREXGLUE_SWITCH_NVK_SDK=$(cygpath -m "$MESA_SDK")" \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DBO2ZM_BUILD_ID=${BO2ZM_BUILD_ID:-switch-$(date +%Y%m%d-%H%M%S)}" \
  "-DBO2ZM_NRO_ICON=$NRO_ICON" \
  "-DCMAKE_EXE_LINKER_FLAGS=-specs=$SPECS -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax$LTO_LINK"
# The toolchain file writes switch.specs into the build folder and passes it
# to GCC unquoted; our project path has spaces, so link from a copy in a
# space-free folder instead (CMAKE_EXE_LINKER_FLAGS above).
mkdir -p "$(dirname "$(cygpath -u "$SPECS")")"
cp "$OUT/switch.specs" "$(cygpath -u "$SPECS")"
# The generated code must come from tools/codegen.sh, which adds the fix-ups the build depends on: fail early if a
# hooked function is called directly (__imp__sub_X, so its hook would be skipped) or a setjmp site lacks the
# register-locals fix.
"${PYTHON:-python}" "$APP/../../tools/direct_calls.py" --check --gen "$APP/generated/default_mp"
"${PYTHON:-python}" "$APP/../../tools/setjmp_locals.py" --verify --gen "$APP/generated/default_mp"
# Plain guest memory is only safe with the barriers in place (core/bo2zm_guest_memory.h).
if [ "${BO2_GUEST_MEMORY_PLAIN:-1}" = "1" ]; then
  "${PYTHON:-python}" "$APP/../../tools/guest_barriers.py" --check --gen "$APP/generated/default_mp"
fi
# A profile trained before the generated code or its flags changed stops applying without a word, so say so before
# building with it. Not fatal: an old profile only loses the gain.
if [ "${BO2_PGO:-}" = "use" ]; then
  "${PYTHON:-python}" "$APP/../../tools/pgo_check.py" --build "$(cygpath -m "$OUT")" \
    --pgo "$(cygpath -m "${BO2_PGO_DIR:-${BO2_DATA}/pgo}")" ||
    echo "WARNING: building with a stale PGO profile" >&2
fi
# The generated code is big: limit parallel jobs (RAM).
cmake --build "$OUT" --target bo2zm -j "${JOBS:-8}"
ls -la "$OUT/"*.nro
if [ -n "${BO2_BUILD_TAG:-}" ]; then
  exit 0  # builds of other checkouts leave the Ryujinx install alone
fi
# Keep the Ryujinx emulator's test install current, when there is one.
RYU_SD="${BO2_DATA}/ryujinx/portable/sdcard/switch/bo2zm"
if [ -d "$RYU_SD" ]; then
  cp "$OUT/bo2zm.nro" "$RYU_SD/bo2zm.nro"
  echo "copied to Ryujinx: $RYU_SD"
fi
