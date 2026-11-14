#!/usr/bin/env bash
# Prepares a git worktree of bo2-zm-nx so it can build and run next to the main checkout (parallel work).
#
#   tools/bootstrap_worktree.sh <tag> [--own-generated]
#   export BO2_BUILD_TAG=<tag>     # then games/bo2/build_windows.sh, games/bo2/build_switch.sh, games/bo2/run_windows.sh as usual
#
# The SDK is part of the repository (sdk/), so every worktree has its own copy of it and SDK changes are ordinary
# commits on the worktree's branch. Everything git ignores comes from the main checkout:
#   refs/              a junction to the main checkout's refs (read-only use).
#   sdk/thirdparty/*   junctions to the main checkout's fetched third-party sources (tools/fetch_thirdparty.py). The
#                      few tracked files in sdk/thirdparty stay this worktree's own.
#   games/bo2/generated/     a junction to the main checkout's generated C++ (read-only). With --own-generated, a copy, for
#                      work that runs the code generator.
#   work/              a junction (the decrypted image, read-only).
# BO2_BUILD_TAG makes the builds use games/bo2/out/{win,switch}-<tag> with their own copy of the SDK's libraries, so two
# checkouts never write the same file. The PC run needs the shader library next to the exe: it is hard-linked from
# the main checkout's games/bo2/out/win. Game data stays on E: (read-only), as in the main checkout.
set -euo pipefail
TAG="${1:?usage: tools/bootstrap_worktree.sh <tag> [--own-generated]}"
shift
OWN_GEN=0
for a in "$@"; do
  case "$a" in
    --own-generated) OWN_GEN=1 ;;
    *) echo "unknown option $a" >&2; exit 2 ;;
  esac
done
WT="$(cd "$(dirname "$0")/.." && pwd)"
MAIN="$(cygpath -u "$(git -C "$WT" worktree list --porcelain | sed -n 's/^worktree //p' | head -n 1)")"
if [ "$MAIN" = "$WT" ]; then
  echo "this is the main checkout: nothing to do" >&2
  exit 1
fi
junction() {  # junction <link> <target>
  [ -e "$1" ] && return 0
  mkdir -p "$(dirname "$1")"
  # PowerShell, not cmd's mklink: the project path has spaces, which cmd //c cannot be given quoted from Git Bash.
  powershell -NoProfile -Command "New-Item -ItemType Junction -Path '$(cygpath -w "$1")' -Target '$(cygpath -w "$2")'" > /dev/null
  echo "linked $1 -> $2"
}

junction "$WT/refs" "$MAIN/refs"

# The SDK's fetched third-party sources. A folder that also has tracked files (FFmpeg, ffmpeg-overlay) gets its
# untracked contents linked one entry at a time, so the tracked files stay this worktree's; at any depth: FFmpeg's
# libavcodec has two tracked files of its own, and linking only the first level left it nearly empty (CMake: "Cannot
# find source file ... libavcodec/ac3_parser.c").
fill() {  # fill <folder, relative to the checkout, with tracked files below it>
  local rel="$1" e sub
  for e in "$MAIN/$rel"/* "$MAIN/$rel"/.git; do
    [ -e "$e" ] || continue
    sub="$rel/$(basename "$e")"
    if [ -n "$(git -C "$WT" ls-files -- "$sub" | head -n 1)" ]; then
      if [ -d "$e" ] && [ ! -L "$WT/$sub" ]; then fill "$sub"; fi
      continue
    fi
    if [ -d "$e" ]; then junction "$WT/$sub" "$e"; else [ -e "$WT/$sub" ] || cp "$e" "$WT/$sub"; fi
  done
}
for d in "$MAIN"/sdk/thirdparty/*/; do
  d="${d%/}"
  name="$(basename "$d")"
  if [ -n "$(git -C "$WT" ls-files -- "sdk/thirdparty/$name" | head -n 1)" ]; then
    fill "sdk/thirdparty/$name"
  else
    junction "$WT/sdk/thirdparty/$name" "$d"
  fi
done

# generated code and the decrypted image
if [ "$OWN_GEN" = 1 ]; then
  if [ ! -e "$WT/games/bo2/generated" ]; then
    cp -r "$MAIN/games/bo2/generated" "$WT/games/bo2/generated"
    echo "copied games/bo2/generated"
  fi
else
  junction "$WT/games/bo2/generated" "$MAIN/games/bo2/generated"
fi
junction "$WT/work" "$MAIN/work"
# This machine's data folder (tools/data_dir.sh), so the worktree's scripts find the same one.
[ -s "$MAIN/bo2_data.txt" ] && [ ! -e "$WT/bo2_data.txt" ] && cp "$MAIN/bo2_data.txt" "$WT/bo2_data.txt"

# the PC run's shader library
mkdir -p "$WT/games/bo2/out/win-$TAG"
if [ ! -e "$WT/games/bo2/out/win-$TAG/bo2zm_shaders.spvpack" ]; then
  ln "$MAIN/games/bo2/out/win/bo2zm_shaders.spvpack" "$WT/games/bo2/out/win-$TAG/bo2zm_shaders.spvpack"
  echo "hard-linked the shader library into games/bo2/out/win-$TAG"
fi
echo
echo "ready. In every shell of this worktree: export BO2_BUILD_TAG=$TAG"
