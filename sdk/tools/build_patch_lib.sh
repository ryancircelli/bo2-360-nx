#!/bin/sh
# Rebuild only a game's patch library (seconds, no relink of the game).
# usage: build_patch_lib.sh OUT.dylib SDK_INCLUDE_DIR [extra clang++ flags / sources...]
# Run it under buildq (nice 15). A running game with patch_reload=true picks the
# new file up within ~0.5 s and binds it at the next frame.
set -e
out="$1"; inc="$2"; shift 2
exec nice -n 15 clang++ -std=c++20 -dynamiclib -fvisibility=hidden -I"$inc" "$@" -o "$out"
