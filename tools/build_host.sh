#!/usr/bin/env bash
# Builds the ReXGlue host tools (rexglue.exe codegen + our xexdump) from the SDK in sdk/, on Windows with Git Bash.
#
# Needs: Kitware CMake, Ninja, LLVM clang (>= 20), Python, git.
# Run from anywhere: tools/build_host.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$ROOT/sdk"
. "$ROOT/tools/toolchain.sh"  # CMake, Ninja and LLVM

# 1. Third-party sources (the official ReXGlue v0.10.0 submodules, copied without .git).
if [ ! -d "$SDK/thirdparty/cli11/include" ]; then
  (cd "$ROOT" && python tools/fetch_thirdparty.py)
fi

# 2. The SDK refuses to configure unless each vendored submodule has a .git
#    entry; the fetch script strips them. An empty .git directory satisfies
#    the check and git ignores it.
for d in "$SDK"/thirdparty/*/; do [ -e "$d.git" ] || mkdir "$d.git"; done

# 3. Windows checkouts turn symlinks (libmspack) into text files holding the
#    target path. Replace them with copies of their targets.
python -I - "$SDK/thirdparty" <<'PYEOF'
import os, shutil, sys
root, fixed = sys.argv[1], 0
for _ in range(3):
    for dp, dn, fn in os.walk(root):
        if '.git' in dp.split(os.sep):
            continue
        for n in fn:
            p = os.path.join(dp, n)
            try:
                if os.path.getsize(p) > 300:
                    continue
                t = open(p, 'rb').read().decode('ascii')
            except Exception:
                continue
            if not t or '\n' in t or t.startswith('#'):
                continue
            tgt = os.path.normpath(os.path.join(dp, t))
            if os.path.isfile(tgt) and os.path.getsize(tgt) > 300:
                shutil.copyfile(tgt, p); fixed += 1
            elif os.path.isdir(tgt):
                os.remove(p); shutil.copytree(tgt, p); fixed += 1
print(f'symlink stubs replaced: {fixed}')
PYEOF

# 4. Configure. GIT_EXECUTABLE points nowhere so the SDK's version check uses its floor (0.10).
#    -march=x86-64-v2 matches the SDK presets (SSSE3 byte swaps need it).
#    The project include hook adds tools/xexdump to the same build.
cmake -S "$SDK" -B "$ROOT/out/host" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
  -DGIT_EXECUTABLE=C:/no-git-here/git.exe \
  "-DCMAKE_PROJECT_ReXGlue_INCLUDE=$ROOT/cmake/host_tools_inject.cmake"

cmake --build "$ROOT/out/host" --target rexglue xexdump -j 12

echo
echo "rexglue: $SDK/out/win-amd64/rexglue.exe"
echo "xexdump: $ROOT/out/host/switch_bo2/xexdump.exe"
echo "Both need $SDK/out/win-amd64 on PATH (runtime DLLs)."
