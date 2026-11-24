# Need for Speed: Most Wanted (Xbox 360, USA), recompiled with the worktree SDK.
#
# Two builds from this file:
#   - the app (default): modeled on tools/build_mac.sh's app step
#       cmake -S games/nfsmw -B games/nfsmw/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#         -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
#     then `buildq -- cmake --build games/nfsmw/out/mac`. The executable is nfsmw; it loads the
#     game from --game_data_root or ~/Games/Xbox360/nfsmw/disc (read-only).
#   - compile-only harness (the first recompile attempt): add -DNFSMW_COMPILE_ONLY=ON. No app,
#     no renderer; just "the generated C++ compiles" as an object library.
cmake_minimum_required(VERSION 3.24)
project(nfsmw CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(NFSMW_COMPILE_ONLY "Build the generated code as an object library only (no app)" OFF)

if(NFSMW_COMPILE_ONLY)
    set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
        include("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sources.cmake")
    else()
        message(FATAL_ERROR "generated/default/sources.cmake missing: run rexglue codegen on nfsmw_manifest.toml first")
    endif()

    # Compile-only harness: the generated code as an object library.
    add_library(nfsmw_recomp OBJECT ${GENERATED_SOURCES})
    target_include_directories(nfsmw_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default")
    target_link_libraries(nfsmw_recomp PRIVATE rex::runtime)
    # Guest memory + REX_GUEST_BARRIER, after the generated PCH (the BO2 app appends this to its PCH).
    target_compile_options(nfsmw_recomp PRIVATE "-include${CMAKE_CURRENT_SOURCE_DIR}/nfsmw_guest_memory.h")
    # Guest SEH scopes emit __try/__except; clang spells the flag the GNU way.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(nfsmw_recomp PRIVATE -fasync-exceptions)
    endif()
    return()
endif()

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

# rex_app.cpp (compiled into this target) includes imgui.h, but rexruntime links
# imgui privately, so the include path does not reach us when the SDK is a
# subdirectory (same for renderdoc_app.h and the Vulkan headers; BO2 app's CMakeLists).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(nfsmw PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(nfsmw PRIVATE Vulkan::Headers)
endif()

if(TARGET nfsmw_recomp)
    # How the generated code reaches guest memory: appended to its precompiled
    # header (after the generated PCH), so REX_GUEST_BARRIER() is defined.
    target_precompile_headers(nfsmw_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/nfsmw_guest_memory.h")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(nfsmw_recomp PRIVATE -fasync-exceptions)
    endif()
    # macOS: the function registration table takes Apple clang ~forever at -O3;
    # at -O0 without the PCH it builds in seconds and runs as fast (BO2 app's trick).
    if(APPLE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/nfsmw_register.cpp"
                                    TARGET_DIRECTORY nfsmw_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
