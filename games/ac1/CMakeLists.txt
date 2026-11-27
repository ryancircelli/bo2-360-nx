# Need for Speed: Hot Pursuit (Xbox 360, USA, 2010), recompiled with the worktree SDK.
#   cmake -S games/ac1 -B games/ac1/out/mac -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF \
#     -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_METAL=ON -DREXSDK_DIR=$PWD/sdk
# The launcher (default.xex) is the main recompiled image; the game itself (ALASKA.XEX.DLL) is a guest DLL module
# the codegen builds as the shared library ac1_game_module (generated/default/dll_targets.cmake).
cmake_minimum_required(VERSION 3.24)
project(ac1 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# The generated code is made once by tools/criterion_codegen.sh (codegen plus the post passes), never by the build:
# with host tools off there is no rexglue target, so the DLL module's dependency on ac1_codegen is met by a no-op.
include(generated/rexglue.cmake)
# Cross builds get no codegen target from rexglue.cmake; keep the name dependable.
if(NOT TARGET ac1_codegen)
    add_custom_target(ac1_codegen)
endif()

add_executable(ac1 src/main.cpp)
rexglue_setup_target(ac1 GPU_PLUGINS metal xenos)
rexglue_game_manifest(ac1 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(ac1 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(ac1 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(ac1 PRIVATE Vulkan::Headers)
endif()

# Guest memory + REX_GUEST_BARRIER, appended to each generated precompiled header.
foreach(t ac1_recomp ac1_game_module)
    if(TARGET ${t})
        target_precompile_headers(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ac1_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${t} PRIVATE -fasync-exceptions)
        endif()
    endif()
endforeach()
# macOS: the function registration table takes Apple clang ~forever at -O3; -O0 without the PCH builds in seconds.
if(APPLE)
    if(TARGET ac1_recomp)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/ac1_register.cpp"
                                    TARGET_DIRECTORY ac1_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
