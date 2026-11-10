#!/usr/bin/env bash
# Builds bo2zm for macOS on Apple silicon from your own copy of the game, start to finish:
#
#   tools/build_mac.sh [--from <step> | --only <step>[,<step>...]]
#
# Steps, in order (--from skips the ones before):
#   host     rexglue and xexdump for this Mac (out/host-mac)
#   data     the disc image and the title update into $BO2_BUILD/data (game/ and update/)
#   dump     the decrypted, TU18-patched default_mp image (work/dump_tu18), which the shader step needs
#   codegen  tools/codegen.sh: the generated code and its fix-up passes ($BO2_BUILD/generated)
#   shaders  tools/mac/build_shaders.py: bo2zm_shaders.spvpack and its index
#   app      the game itself ($BO2_BUILD/out/mac-arm64)
#   package  a folder ready to play ($BO2_BUILD/run, with a bo2zm.command launcher)
#
# Everything made from the game (extracted files, generated code, shaders, the build) goes to $BO2_BUILD,
# ~/Games/Xbox360/bo2-build by default, never into this repository: work/ and app/generated are links to it.
#
# Inputs (override with the variables):
#   BO2_ISO   the disc image (default: ~/Games/Xbox360/roms/Call of Duty - Black Ops II (World).iso)
#   BO2_TU    the Title Update 18 package (default: the only file in $BO2_BUILD/TU/), as dumped from a console
#             (an STFS/LIVE package holding default_mp.xexp). Its default_mp.xexp must be TU18's.
#   VULKAN_SDK the LunarG Vulkan SDK's macOS folder, for dxc (default: the newest ~/VulkanSDK/*/macOS)
# Needs Xcode (clang), and from Homebrew: cmake ninja python3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BO2_BUILD="${BO2_BUILD:-$HOME/Games/Xbox360/bo2-build}"
BO2_ISO="${BO2_ISO:-$HOME/Games/Xbox360/roms/Call of Duty - Black Ops II (World).iso}"
DATA="$BO2_BUILD/data"
OUT="$BO2_BUILD/out/mac-arm64"
HOST="$ROOT/out/host-mac"
RUN="$BO2_BUILD/run"
PY="${PYTHON:-python3}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
  echo "tools/build_mac.sh is for macOS on Apple silicon" >&2
  exit 1
fi
FROM=host
ONLY=""
if [ "${1:-}" = --from ]; then
  FROM="${2:?--from needs a step name}"
elif [ "${1:-}" = --only ]; then
  ONLY="${2:?--only needs step names}"
  FROM="${ONLY%%,*}"
fi
STEPS=(host data dump codegen shaders app package)
started=0
want() {
  if [ -n "$ONLY" ]; then
    case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac
  fi
  [ "$1" = "$FROM" ] && started=1
  [ "$started" = 1 ]
}
case " ${STEPS[*]} " in *" $FROM "*) ;; *) echo "unknown step: $FROM (one of: ${STEPS[*]})" >&2; exit 1 ;; esac
for t in cmake ninja clang++ git; do
  command -v "$t" > /dev/null || { echo "$t not found (brew install cmake ninja; Xcode for clang and git)" >&2; exit 1; }
done
if [ -z "${VULKAN_SDK:-}" ]; then
  VULKAN_SDK="$(ls -d "$HOME"/VulkanSDK/*/macOS 2> /dev/null | sort -V | tail -n 1 || true)"
fi
export VULKAN_SDK

mkdir -p "$BO2_BUILD/work" "$BO2_BUILD/generated" "$DATA"
# Links from the checkout to the data folder (both ignored by git). A real folder in their place is left alone.
for pair in "work:$BO2_BUILD/work" "app/generated:$BO2_BUILD/generated"; do
  link="$ROOT/${pair%%:*}"
  target="${pair#*:}"
  if [ -L "$link" ] || [ ! -e "$link" ]; then
    ln -sfn "$target" "$link"
  elif [ "$(cd "$link" && pwd -P)" != "$(cd "$target" && pwd -P)" ]; then
    echo "$link is a real folder: move it away so it can link to $target" >&2
    exit 1
  fi
done
echo "$DATA" > "$ROOT/bo2_data.txt"

if want host; then
  echo "== host tools"
  [ -d "$ROOT/sdk/thirdparty/cli11/include" ] || "$PY" "$ROOT/tools/fetch_thirdparty.py"
  # The SDK's version script expects a .git in each third-party folder.
  for d in "$ROOT"/sdk/thirdparty/*/; do
    [ -e "$d/.git" ] || mkdir -p "$d/.git"
  done
  cmake -S "$ROOT/sdk" -B "$HOST" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_C_FLAGS=-march=armv8-a -DCMAKE_CXX_FLAGS=-march=armv8-a \
    "-DREXGLUE_OUTPUT_DIR=$HOST/bin" "-DCMAKE_PROJECT_ReXGlue_INCLUDE=$ROOT/cmake/host_tools_inject.cmake" > /dev/null
  cmake --build "$HOST" -j "$JOBS" --target rexglue xexdump
fi
XEXDUMP="$HOST/switch_bo2/xexdump"

if want data; then
  echo "== game files"
  if [ ! -f "$DATA/game/default_mp.xex" ]; then
    [ -f "$BO2_ISO" ] || { echo "disc image not found: $BO2_ISO (set BO2_ISO)" >&2; exit 1; }
    mkdir -p "$DATA/game"
    "$PY" -I -c 'import sys; sys.path.insert(0, sys.argv[1]); from xiso import Xiso
with Xiso(sys.argv[2]) as x: x.extract(sys.argv[3])' "$ROOT/tools/install" "$BO2_ISO" "$DATA/game"
  fi
  if [ ! -f "$DATA/update/default_mp.xexp" ]; then
    if [ -z "${BO2_TU:-}" ]; then
      BO2_TU="$(find "$BO2_BUILD/TU" -maxdepth 1 -type f 2> /dev/null | head -n 1)"
    fi
    [ -n "$BO2_TU" ] && [ -f "$BO2_TU" ] ||
      { echo "Title Update 18 not found: put the package in $BO2_BUILD/TU/ (or set BO2_TU)" >&2; exit 1; }
    mkdir -p "$DATA/update"
    "$PY" -I "$ROOT/tools/inspect/stfs_extract.py" "$BO2_TU" --out "$DATA/update" > /dev/null
  fi
  "$PY" -I - "$ROOT/tools/install" "$DATA" << 'EOF'
import os, sys
sys.path.insert(0, sys.argv[1])
import game_files
data = sys.argv[2]
if game_files.sha256(os.path.join(data, "game", "default_mp.xex")) != game_files.DISC_XEX_SHA256:
    sys.exit("game/default_mp.xex is not the Xbox 360 disc version this port is made for")
if game_files.sha256(os.path.join(data, "update", "default_mp.xexp")) != game_files.TU18_XEXP_SHA256:
    sys.exit("update/default_mp.xexp is not Title Update 18's")
print("disc and Title Update 18 verified")
EOF
  # The game loads the patch from beside the executable, as on the console's hard drive.
  cp "$DATA/update/default_mp.xexp" "$DATA/update/default.xexp" "$DATA/game/"
fi

if want dump; then
  echo "== TU18 image"
  mkdir -p "$ROOT/work/load_tu18"
  cp "$DATA/game/default_mp.xex" "$DATA/game/default_mp.xexp" "$ROOT/work/load_tu18/"
  "$XEXDUMP" "$ROOT/work/load_tu18" default_mp.xex "$ROOT/work/dump_tu18" | tail -n 1
fi

if want codegen; then
  echo "== codegen"
  BO2_GENERATED_LINK_OK=1 REXGLUE_BIN_DIR="$HOST/bin" PYTHON="$PY" bash "$ROOT/tools/codegen.sh"
fi

if want shaders; then
  echo "== shaders"
  [ -x "$VULKAN_SDK/bin/dxc" ] ||
    { echo "dxc not found: install the LunarG Vulkan SDK for macOS (vulkan.lunarg.com) or set VULKAN_SDK" >&2; exit 1; }
  "$PY" -I "$ROOT/tools/mac/build_shaders.py" --data "$DATA" --image "$ROOT/work/dump_tu18/default_mp.image.bin" \
    --work "$BO2_BUILD/shaders-work" --out "$BO2_BUILD/shaders"
fi

if want app; then
  echo "== app"
  [ -f "$ROOT/app/generated/default_mp/sources.cmake" ] || { echo "no generated code: run the codegen step" >&2; exit 1; }
  FLAGS="-march=armv8-a"
  RELEASE="-O3 -DNDEBUG"
  cmake -S "$ROOT/app" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    "-DCMAKE_C_FLAGS=$FLAGS" "-DCMAKE_CXX_FLAGS=$FLAGS" \
    "-DCMAKE_C_FLAGS_RELEASE=$RELEASE" "-DCMAKE_CXX_FLAGS_RELEASE=$RELEASE" \
    "-DCMAKE_BUILD_RPATH=@executable_path" \
    "-DREXGLUE_OUTPUT_DIR=$OUT/sdk-out" \
    -DREXGLUE_BUILD_HOST_TOOLS=OFF -DREXGLUE_USE_VULKAN=ON -DBO2ZM_GUEST_MEMORY_PLAIN=ON \
    "-DBO2ZM_BUILD_ID=mac-arm64-$(date +%Y%m%d-%H%M%S)" "-DREXSDK_DIR=$ROOT/sdk" > /dev/null
  G="$ROOT/app/generated/default_mp"
  "$PY" "$ROOT/tools/direct_calls.py" --check --gen "$G"
  "$PY" "$ROOT/tools/setjmp_locals.py" --verify --gen "$G"
  "$PY" "$ROOT/tools/guest_barriers.py" --check --gen "$G"
  cmake --build "$OUT" -j "$JOBS"
fi

if want package; then
  echo "== package"
  bash "$ROOT/tools/mac/package.sh" "$OUT" "$RUN" "$BO2_BUILD/shaders" "$DATA"
  echo "ready: open $RUN/bo2zm.command (or run it from a terminal)"
fi
