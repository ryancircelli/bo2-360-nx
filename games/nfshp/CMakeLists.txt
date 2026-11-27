# Need for Speed: Hot Pursuit (Xbox 360, USA, 2010), recompiled with the worktree SDK.
#   cmake -S games/nfshp -B games/nfshp/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
# The launcher (default.xex) is the main recompiled image; the game itself (ALASKA.XEX.DLL) is a guest DLL module
# the codegen builds as the shared library nfshp_game_module (generated/default/dll_targets.cmake).
cmake_minimum_required(VERSION 3.24)
project(nfshp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# The generated code is made once by tools/criterion_codegen.sh (codegen plus the post passes), never by the build:
# with host tools off there is no rexglue target, so the DLL module's dependency on nfshp_codegen is met by a no-op.
if(NOT TARGET nfshp_codegen)
    add_custom_target(nfshp_codegen)
endif()
include(generated/rexglue.cmake)

add_executable(nfshp)
rexglue_setup_target(nfshp GPU_PLUGINS metal xenos)
rexglue_game_manifest(nfshp "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

rexglue_game_defaults(nfshp MAIN)
