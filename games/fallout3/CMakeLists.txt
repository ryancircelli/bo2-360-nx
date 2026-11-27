# Fallout 3 (Xbox 360, GOTY disc, 2008), recompiled with the worktree SDK.
#   cmake -S games/fallout3 -B games/fallout3/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
cmake_minimum_required(VERSION 3.24)
project(fallout3 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")

include(generated/rexglue.cmake)

add_executable(fallout3)
rexglue_setup_target(fallout3 GPU_PLUGINS metal xenos)
rexglue_game_manifest(fallout3 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")
rexglue_game_defaults(fallout3 MAIN)
