#!/usr/bin/env bash
# Finds the build tools every build script needs, so none of them hard-codes where a tool is installed. Source it
# from a script (". tools/toolchain.sh") after setting ROOT; it only sets variables and PATH.
#
# Each tool can be named with an environment variable; without one, the usual install location is tried, then PATH:
#
#   BO2_CMAKE_DIR    folder with Kitware's cmake.exe   (default C:/Program Files/CMake/bin when it exists). It goes
#                    first on PATH.
#   BO2_NINJA        ninja.exe                         (default: winget's install folder, else ninja on PATH)
#   BO2_LLVM_DIR     folder with clang and clang++     (default C:/Program Files/LLVM/bin when it exists)
#   VULKAN_SDK       the Vulkan SDK                    (default: the newest C:/VulkanSDK/<version>)
#
# Every path it sets is in the mixed form (C:/...) that Windows programs and CMake accept, except PATH entries.

_bo2_unix() { cygpath -u "$1" 2>/dev/null || echo "$1"; }
_bo2_mixed() { cygpath -m "$1" 2>/dev/null || echo "$1"; }

# CMake: Kitware's, ahead of anything else on PATH.
if [ -n "${BO2_CMAKE_DIR:-}" ]; then
  export PATH="$(_bo2_unix "$BO2_CMAKE_DIR"):$PATH"
elif [ -d "/c/Program Files/CMake/bin" ]; then
  export PATH="/c/Program Files/CMake/bin:$PATH"
fi

# Ninja. The Switch toolchain file limits program lookups to the target sysroot, so the scripts also pass it to CMake
# by name (NINJA_EXE).
NINJA_EXE=""
if [ -n "${BO2_NINJA:-}" ]; then
  NINJA_EXE="$(_bo2_mixed "$BO2_NINJA")"
else
  for _bo2_n in "$(_bo2_unix "${LOCALAPPDATA:-}")"/Microsoft/WinGet/Packages/Ninja-build.Ninja_*/ninja.exe; do
    [ -f "$_bo2_n" ] && NINJA_EXE="$(_bo2_mixed "$_bo2_n")"
  done
  if [ -z "$NINJA_EXE" ] && command -v ninja > /dev/null 2>&1; then
    NINJA_EXE="$(_bo2_mixed "$(command -v ninja)")"
  fi
fi
if [ -n "$NINJA_EXE" ]; then
  export PATH="$(dirname "$(_bo2_unix "$NINJA_EXE")"):$PATH"
fi

# LLVM (the PC build, the host tools, the shader translator).
if [ -n "${BO2_LLVM_DIR:-}" ]; then
  export PATH="$(_bo2_unix "$BO2_LLVM_DIR"):$PATH"
elif [ -d "/c/Program Files/LLVM/bin" ]; then
  export PATH="/c/Program Files/LLVM/bin:$PATH"
fi

# The Vulkan SDK: the newest one installed, unless VULKAN_SDK names one.
if [ -z "${VULKAN_SDK:-}" ]; then
  for _bo2_v in /c/VulkanSDK/*/; do
    [ -d "$_bo2_v" ] && VULKAN_SDK="${_bo2_v%/}"
  done
fi
VULKAN_SDK="${VULKAN_SDK:+$(_bo2_unix "$VULKAN_SDK")}"
unset _bo2_n _bo2_v
