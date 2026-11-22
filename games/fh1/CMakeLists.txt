# Forza Horizon (Xbox 360, USA), recompiled with the worktree SDK.
#
#   cmake -S games/fh1 -B games/fh1/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
#   buildq -- cmake --build games/fh1/out/mac
cmake_minimum_required(VERSION 3.24)
project(fh1 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
include(generated/rexglue.cmake)

add_executable(fh1 src/main.cpp hook_stubs.cpp patch_hooks.cpp)
rexglue_setup_target(fh1 GPU_PLUGINS xenos metal)
rexglue_game_manifest(fh1 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(fh1 PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(fh1 PRIVATE Vulkan::Headers)
endif()

if(TARGET fh1_recomp)
    target_precompile_headers(fh1_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fh1_guest_memory.h")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(fh1_recomp PRIVATE -fasync-exceptions)
    endif()
    if(APPLE)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fh1_register.cpp"
            TARGET_DIRECTORY fh1_recomp
            PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fh1_recomp.196.cpp"
            TARGET_DIRECTORY fh1_recomp
            PROPERTIES COMPILE_OPTIONS "-O1")
    endif()
endif()
