# Need for Speed: Most Wanted 2012 (Xbox 360, 2012), recompiled with the worktree SDK.
#   cmake -S games/nfsmw2012 -B games/nfsmw2012/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
# The launcher (default.xex) is the main recompiled image; the game itself (HAWAII.XEX.DLL) is a guest DLL module
# the codegen builds as the shared library nfsmw2012_game_module (generated/default/dll_targets.cmake).
cmake_minimum_required(VERSION 3.24)
project(nfsmw2012 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# The generated code is made once by tools/criterion_codegen.sh (codegen plus the post passes), never by the build:
# with host tools off there is no rexglue target, so the DLL module's dependency on nfsmw2012_codegen is met by a no-op.
include(generated/rexglue.cmake)
# Cross builds get no codegen target from rexglue.cmake; keep the name dependable.
if(NOT TARGET nfsmw2012_codegen)
    add_custom_target(nfsmw2012_codegen)
endif()

add_executable(nfsmw2012)
rexglue_setup_target(nfsmw2012 GPU_PLUGINS metal xenos)
rexglue_game_manifest(nfsmw2012 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

rexglue_game_defaults(nfsmw2012 MAIN)
