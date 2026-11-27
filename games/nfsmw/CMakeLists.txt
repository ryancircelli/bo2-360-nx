# Need for Speed: Most Wanted (Xbox 360, USA), recompiled with the worktree SDK.
#
# Build:
#   modeled on tools/build_mac.sh's app step
#       cmake -S games/nfsmw -B games/nfsmw/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#         -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
#     then `buildq -- cmake --build games/nfsmw/out/mac`. The executable is nfsmw; it loads the
#     game from --game_data_root or ~/Games/Xbox360/nfsmw/disc (read-only).
cmake_minimum_required(VERSION 3.24)
project(nfsmw CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The app. generated/rexglue.cmake (written by codegen) finds the SDK via the
# REXSDK_DIR cache path and add_subdirectory()s it, then defines
# rexglue_setup_target, which builds the generated code as the nfsmw_recomp
# object library and links it in.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
include(generated/rexglue.cmake)

add_executable(nfsmw src/main.cpp)
# GPU_PLUGINS: the native Metal renderer (game.toml's default) and the emulated Xbox 360 GPU, loaded
# at runtime.
rexglue_setup_target(nfsmw GPU_PLUGINS metal xenos)
rexglue_patchable_all(nfsmw_recomp)   # test builds: -DREXGLUE_PATCHABLE_ALL=ON (default OFF)
rexglue_game_manifest(nfsmw "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

rexglue_game_defaults(nfsmw)
