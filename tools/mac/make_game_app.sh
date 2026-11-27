#!/usr/bin/env bash
# Builds one game of games/ and packs it into its app, in one command:
#
#   tools/mac/make_game_app.sh <game> [--install] [--no-build] [--no-pack] [--build-dir DIR] [--out APP]
#
# <game> is a folder of games/ with an app.toml (nfsmw, fh1, bo1). Its game facts (title id, executable, renderer
# defaults) stay in games/<game>/game.toml; games/<game>/app.toml names the app, its saves folder and its game data
# folder (~/Games/Xbox360/<game>/disc by default: read in place, never copied into the app or the repository).
#   --build-dir  the CMake build folder (default: $REX_BUILD_ROOT/apps-<game>, REX_BUILD_ROOT defaulting to
#                ~/Games/Xbox360/bo2-build/out). Configured on first use, then built incrementally.
#   --no-build   pack what the build folder already has
#   --no-pack    build only (tools/mac/build_app.py packs the app itself, with the game data)
#   --out        the app to write (default: $REX_APP_DIR/<name>.app, REX_APP_DIR defaulting to the build folder)
#   --install    write it to ~/Games/Launchers/<name>.app instead (replaced atomically if it exists)
# Builds go through buildq when it is there ($BUILDQ, default ~/Games/Xbox360/bo2-build/runq/bin/buildq), else
# straight through cmake. Needs the game's generated code (games/<game>/generated) and default.xex in place, as for
# any build of that game; the code generator is not rerun here.
# Black Ops II keeps its own route: tools/build_mac.sh, then tools/mac/pack_app.py with its defaults.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GAME="${1:?usage: tools/mac/make_game_app.sh <game> [--install] [--no-build] [--no-pack] [--build-dir DIR] [--out APP]}"
shift
BUILD=1 PACK=1 INSTALL=0 OUT_APP="" BDIR=""
while [ $# -gt 0 ]; do
  case "$1" in
    --install) INSTALL=1 ;;
    --no-build) BUILD=0 ;;
    --no-pack) PACK=0 ;;
    --build-dir) BDIR="$2"; shift ;;
    --out) OUT_APP="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
G="$ROOT/games/$GAME"
[ -f "$G/app.toml" ] || { echo "no games/$GAME/app.toml" >&2; exit 1; }
PY="${PYTHON:-python3}"
field() { "$PY" -I -c 'import sys,tomllib; v=tomllib.load(open(sys.argv[1],"rb")).get(sys.argv[2],""); print("\n".join(v) if isinstance(v,list) else v)' "$G/app.toml" "$1"; }
NAME="$(field name)"
BDIR="${BDIR:-${REX_BUILD_ROOT:-$HOME/Games/Xbox360/bo2-build/out}/apps-$GAME}"
BQ="${BUILDQ:-$HOME/Games/Xbox360/bo2-build/runq/bin/buildq}"
[ -x "$BQ" ] && RUNB=("$BQ" --) || RUNB=()

if [ "$BUILD" = 1 ]; then
  [ -f "$G/generated/rexglue.cmake" ] || { echo "no generated code in games/$GAME/generated" >&2; exit 1; }
  if [ ! -f "$BDIR/build.ninja" ]; then
    echo "== configure $BDIR"
    EXTRA=()
    while IFS= read -r a; do [ -n "$a" ] && EXTRA+=("$a"); done < <(field cmake_args)
    # Host tools off: no rexglue target, so the build never reruns codegen (that would drop the fix-up passes).
    cmake -S "$G" -B "$BDIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
      "-DCMAKE_C_FLAGS=-march=armv8-a" "-DCMAKE_CXX_FLAGS=-march=armv8-a" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DREXGLUE_USE_METAL=ON -DREXGLUE_USE_VULKAN=ON -DREXGLUE_BUILD_TESTS=OFF "-DREXSDK_DIR=$ROOT/sdk" \
      -DREXGLUE_BUILD_HOST_TOOLS=OFF \
      ${EXTRA[@]+"${EXTRA[@]}"} > "$BDIR.configure.log" 2>&1 || { cat "$BDIR.configure.log" >&2; exit 1; }
  fi
  echo "== build $BDIR"
  ${RUNB[@]+"${RUNB[@]}"} cmake --build "$BDIR"
fi

[ "$PACK" = 1 ] || exit 0
if [ "$INSTALL" = 1 ]; then
  OUT_APP="$HOME/Games/Launchers/$NAME.app"
  mkdir -p "$HOME/Games/Launchers"
fi
OUT_APP="${OUT_APP:-${REX_APP_DIR:-$BDIR}/$NAME.app}"
echo "== pack $OUT_APP"
"$PY" -I "$ROOT/tools/mac/pack_app.py" --game "$GAME" --build "$BDIR" --out "$OUT_APP"
