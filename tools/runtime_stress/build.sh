#!/usr/bin/env bash
# Builds and runs the waits stress test (games/bo2/src/bo2zm_test_waits.cpp) on Linux, against the SDK's own
# threading_posix.cpp. Run it inside WSL (or any Linux with g++ 13+):
#
#   tools/runtime_stress/build.sh [fast|original] [scale] [out_dir]
#
#   fast      (default) the address-arbiter waits of REX_FAST_WAITS, with Linux futexes standing in for
#             svcWaitForAddress / svcSignalToAddress: the code the Switch build runs, apart from those two calls.
#   original  the SDK's mutex + condition variable implementation, for comparison.
#   scale     iteration multiplier (1.0: about 20 s).
set -euo pipefail
VARIANT="${1:-fast}"
SCALE="${2:-1.0}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
SDK="$ROOT/sdk"
OUT="${3:-$HERE/out}"
mkdir -p "$OUT"
DEFS=()
if [ "$VARIANT" = fast ]; then
  DEFS=(-DREX_FAST_WAITS_TEST)
fi
EXE="$OUT/test_waits_$VARIANT"
g++ -std=c++20 -O2 -g -pthread -Wall -Wextra -Wno-unused-parameter "${DEFS[@]}" \
  -DBO2ZM_TEST_WAITS_STANDALONE -include rex/assert.h \
  -I "$HERE/stub" -I "$SDK/include" -I "$ROOT/games/bo2/src" -I "$ROOT/core" \
  "$SDK/src/core/threading_posix.cpp" "$SDK/src/core/threading.cpp" "$SDK/src/core/timer_queue.cpp" \
  "$SDK/src/core/clock.cpp" "$SDK/src/core/clock_posix.cpp" \
  "$ROOT/games/bo2/src/bo2zm_test_waits.cpp" "$HERE/main.cpp" \
  -o "$EXE"
echo "built $EXE"
"$EXE" "$SCALE"
