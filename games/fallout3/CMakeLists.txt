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
# The generated code is made once by tools/fallout3_codegen.sh (codegen plus the post passes), never by the build:
# with host tools off there is no rexglue target, so the the generated target on fallout3_codegen is met by a no-op.
if(NOT TARGET fallout3_codegen)
    add_custom_target(fallout3_codegen)
endif()
include(generated/rexglue.cmake)

add_executable(fallout3 src/main.cpp)
rexglue_setup_target(fallout3 GPU_PLUGINS metal xenos)
rexglue_game_manifest(fallout3 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(TARGET imgui)
    target_include_directories(fallout3 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET renderdoc)
    target_include_directories(fallout3 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(fallout3 PRIVATE Vulkan::Headers)
endif()

# Guest memory + REX_GUEST_BARRIER, appended to each generated precompiled header.
foreach(t fallout3_recomp)
    if(TARGET ${t})
        target_precompile_headers(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fallout3_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${t} PRIVATE -fasync-exceptions)
        endif()
    endif()
endforeach()
# macOS: the function registration table takes Apple clang ~forever at -O3; -O0 without the PCH builds in seconds.
if(APPLE)
    if(TARGET fallout3_recomp)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/fallout3_register.cpp"
                                    TARGET_DIRECTORY fallout3_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
