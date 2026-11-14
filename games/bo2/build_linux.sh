#!/usr/bin/env bash
# Configure and build bo2zm for AArch64 (ARM64) Linux, then lay out the runtime package. Runs on Linux: an ARM64
# machine builds natively; an x86_64 machine (WSL2, Docker, a desktop) cross-compiles with clang and the sysroot from
# tools/linux/make_sysroot.sh. docs/building-linux-arm64.md is the guide.
#
#   bash games/bo2/build_linux.sh
#
# Environment:
#   BO2_LINUX_SYSROOT  cross builds: the sysroot (default out/linux-sysroot; made on first use). Also usable on an
#                      ARM64 host, to build against exactly the same libraries as a cross build.
#   BO2_LINUX_OUT      build folder (default games/bo2/out/linux-arm64, or games/bo2/out/linux-arm64-$BO2_BUILD_TAG)
#   BO2_LINUX_MARCH    -march for everything (default armv8-a: any 64-bit ARM CPU, Cortex-A53/A72 included)
#   BO2_GUEST_MEMORY_PLAIN  1 (default, as on the Switch) or 0: how the generated code reaches guest memory
#   BO2_LTO=1          ThinLTO across the generated code and the runtime (slower link, more memory)
#   CLANG_SUFFIX       a versioned clang, e.g. -20 for clang-20 (default: the first of clang, clang-22 ... clang-19
#                      that is version 19 or newer)
#   JOBS               parallel compile jobs (default: the CPU count, at most 12; the generated code needs ~1.5 GB
#                      of memory per job)
#
# The generated C++ (games/bo2/generated) and the SDK's third-party sources (sdk/thirdparty) must already be in place:
# the generated code comes from tools/codegen.sh, run on the player's own copy of the game; the third-party sources
# from tools/fetch_thirdparty.py.
set -euo pipefail
APP="$(cd "$(dirname "$0")" && pwd -P)"
ROOT="$(cd "$APP/../.." && pwd)"
SDK="${BO2_SDK_DIR:-$ROOT/sdk}"
OUT="${BO2_LINUX_OUT:-$APP/out/linux-arm64${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}}"
MARCH="${BO2_LINUX_MARCH:-armv8-a}"
HOST_ARCH="$(uname -m)"

if [ "$(uname -s)" != Linux ]; then
  echo "games/bo2/build_linux.sh runs on Linux (on Windows: inside WSL2, see docs/building-linux-arm64.md)" >&2
  exit 1
fi
if [ ! -f "$APP/generated/default_mp/sources.cmake" ]; then
  echo "no generated code in games/bo2/generated/default_mp: run tools/codegen.sh on your copy of the game first" >&2
  exit 1
fi
# Clang 19 or newer: with clang 18, libstdc++'s <expected> stays empty (it tests __cpp_concepts >= 202002L, which
# clang only reports from 19 on) and the SDK does not compile. Without CLANG_SUFFIX the first suitable clang is used.
clang_major() { { "clang$1" -dumpversion 2> /dev/null || true; } | cut -d. -f1; }
if [ -z "${CLANG_SUFFIX+set}" ]; then
  for s in "" -22 -21 -20 -19; do
    v="$(clang_major "$s")"
    if [ -n "$v" ] && [ "$v" -ge 19 ]; then
      export CLANG_SUFFIX="$s"
      break
    fi
  done
fi
v="$(clang_major "${CLANG_SUFFIX:-}")"
if [ -z "$v" ] || [ "$v" -lt 19 ]; then
  echo "clang 19 or newer is needed (found: ${v:-none}); on Ubuntu 24.04: apt install clang-20 lld-20 llvm-20" >&2
  exit 1
fi
command -v "ld.lld${CLANG_SUFFIX:-}" > /dev/null || command -v ld.lld > /dev/null ||
  { echo "lld not found (apt install lld${CLANG_SUFFIX:-})" >&2; exit 1; }

ARGS=()
if [ "$HOST_ARCH" = aarch64 ] && [ -z "${BO2_LINUX_SYSROOT:-}" ]; then
  # Native build with the machine's own clang and libraries.
  ARGS+=("-DCMAKE_C_COMPILER=clang${CLANG_SUFFIX:-}" "-DCMAKE_CXX_COMPILER=clang++${CLANG_SUFFIX:-}")
else
  export BO2ZM_LINUX_SYSROOT="${BO2_LINUX_SYSROOT:-$ROOT/out/linux-sysroot}"
  if [ ! -e "$BO2ZM_LINUX_SYSROOT/usr/include/stdio.h" ]; then
    bash "$ROOT/tools/linux/make_sysroot.sh" "$BO2ZM_LINUX_SYSROOT"
  fi
  ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/linux/cmake/aarch64-linux-gnu.cmake")
fi

# Release as on the Switch (-O3 from the SDK). libstdc++ and libgcc are linked statically, so the binary needs only
# glibc (2.38 or newer with the default sysroot) and the system's Vulkan loader, X11/Wayland and audio libraries,
# which SDL and the SDK open at run time. $ORIGIN: the SDK's shared runtime and GPU plugin sit next to the binary.
FLAGS="-march=$MARCH"
LINK="-fuse-ld=lld -static-libstdc++ -static-libgcc"
RELEASE="-O3 -DNDEBUG"
if [ "${BO2_LTO:-0}" = 1 ]; then
  RELEASE="$RELEASE -flto=thin"
  LINK="$LINK -flto=thin"
fi
# The SDK's shared runtime binds calls to its own functions inside itself, as a Windows DLL does. Without it every such
# call goes through the PLT, and FFmpeg's AArch64 assembly (linked into the runtime) has a conditional branch to its
# own global symbol, ff_prefetch_aarch64, that cannot reach a PLT stub +/-1 MB away in a library this size.
SHARED_LINK="$LINK -Wl,-Bsymbolic-functions"
# The SDK's libraries go into the build folder, never into sdk/ (its default), so package.sh finds them and two
# checkouts or build folders never share them.
SDK_OUT_ARG=("-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out")
JOBS="${JOBS:-$(n=$(nproc); [ "$n" -gt 12 ] && n=12; echo "$n")}"

cmake -S "$APP" -B "$OUT" -G Ninja "${ARGS[@]}" "${SDK_OUT_ARG[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_C_FLAGS=$FLAGS" "-DCMAKE_CXX_FLAGS=$FLAGS" \
  "-DCMAKE_C_FLAGS_RELEASE=$RELEASE" "-DCMAKE_CXX_FLAGS_RELEASE=$RELEASE" \
  "-DCMAKE_EXE_LINKER_FLAGS=$LINK" "-DCMAKE_SHARED_LINKER_FLAGS=$SHARED_LINK" "-DCMAKE_MODULE_LINKER_FLAGS=$SHARED_LINK" \
  "-DCMAKE_BUILD_RPATH=\$ORIGIN" \
  -DREXGLUE_BUILD_HOST_TOOLS=OFF \
  -DREXGLUE_USE_VULKAN=ON \
  "-DBO2ZM_GUEST_MEMORY_PLAIN=$([ "${BO2_GUEST_MEMORY_PLAIN:-1}" = 1 ] && echo ON || echo OFF)" \
  "-DBO2ZM_BUILD_ID=linux-arm64-$(date +%Y%m%d-%H%M%S)" \
  "-DREXSDK_DIR=$SDK"

# Same checks as the other builds: the generated code must come from tools/codegen.sh, with every fix-up applied.
PY="${PYTHON:-python3}"
"$PY" "$ROOT/tools/direct_calls.py" --check --gen "$APP/generated/default_mp"
"$PY" "$ROOT/tools/setjmp_locals.py" --verify --gen "$APP/generated/default_mp"
if [ "${BO2_GUEST_MEMORY_PLAIN:-1}" = 1 ]; then
  "$PY" "$ROOT/tools/guest_barriers.py" --check --gen "$APP/generated/default_mp"
fi

cmake --build "$OUT" -j "$JOBS"
bash "$ROOT/tools/linux/package.sh" "$OUT"
