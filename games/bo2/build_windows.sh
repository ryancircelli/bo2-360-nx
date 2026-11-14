#!/usr/bin/env bash
# Configures and builds bo2zm.exe for Windows with LLVM's clang and Ninja. The PC build runs the same recompiled game
# as the Switch build, drawn by the native Vulkan renderer (bo2zm_renderer=native) or by ReXGlue's emulated Xbox 360
# GPU (xenos), so Vulkan is enabled besides D3D12. The SDK flags are those of tools/build_host.sh. The output is
# games/bo2/out/win/bo2zm.exe; games/bo2/run_windows.sh starts it. Where CMake, Ninja and LLVM are installed comes from
# tools/toolchain.sh; JOBS and BO2ZM_BUILD_ID work as in build_switch.sh.
set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd -P)"
BO2_DATA="$(bash "$APP/../../tools/data_dir.sh")"
SDK="${BO2_SDK_DIR:-$APP/../../sdk}"
# Several checkouts at once (git worktrees prepared by tools/bootstrap_worktree.sh): BO2_BUILD_TAG gives this checkout
# its own build folder and its own copy of the SDK's libraries, so two worktrees never write the same files. Without
# it: games/bo2/out/win.
OUT="$APP/out/win${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}"
# The build never runs the code generator (REXGLUE_BUILD_HOST_TOOLS=OFF): the generated code comes only from
# tools/codegen.sh, which adds the fix-ups the build depends on (the setjmp fix, direct calls). Left on, the build
# would re-run the plain code generator whenever overrides.toml changed and lose them. In a worktree the generated code
# may also be the main checkout's (a junction).
SDK_OUT_ARG=(-DREXGLUE_BUILD_HOST_TOOLS=OFF)
if [ -n "${BO2_BUILD_TAG:-}" ]; then
  SDK_OUT_ARG+=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
fi
# bo2zm.exe icon. It carries the game's logo, so it lives with the game data, outside the repo.
# Missing: an empty value, and the exe has no icon.
EXE_ICON="${BO2ZM_EXE_ICON:-${BO2_DATA}/art/icon_pc.ico}"
if [ -f "$EXE_ICON" ]; then EXE_ICON="$(cygpath -m "$EXE_ICON")"; else EXE_ICON=""; fi
ROOT="$APP/../.."
. "$ROOT/tools/toolchain.sh"
# BO2_DEVLINK=0: a build without the development consoles' PC link (games/bo2/CMakeLists.txt, BO2ZM_DEVLINK).
DEVLINK_ARGS=("-DBO2ZM_DEVLINK=$([ "${BO2_DEVLINK:-1}" = "0" ] && echo OFF || echo ON)")
cmake -S "$APP" -B "$OUT" -G Ninja ${NINJA_EXE:+"-DCMAKE_MAKE_PROGRAM=$NINJA_EXE"} "${SDK_OUT_ARG[@]}" "${DEVLINK_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DBO2ZM_BUILD_ID=${BO2ZM_BUILD_ID:-pc-$(date +%Y%m%d-%H%M%S)}" \
  "-DBO2ZM_EXE_ICON=$EXE_ICON" \
  -DREXGLUE_USE_VULKAN=ON \
  "-DREXSDK_DIR=$SDK"
# The generated code must come from tools/codegen.sh: fail early if a hooked function is called directly
# (__imp__sub_X, so its hook would be skipped) or a setjmp site lacks the register-locals fix.
"${PYTHON:-python}" "$APP/../../tools/direct_calls.py" --check --gen "$APP/generated/default_mp"
"${PYTHON:-python}" "$APP/../../tools/setjmp_locals.py" --verify --gen "$APP/generated/default_mp"
cmake --build "$OUT" --target bo2zm -j "${JOBS:-12}"
