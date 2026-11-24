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

add_executable(nfshp src/main.cpp)
rexglue_setup_target(nfshp GPU_PLUGINS metal xenos)
rexglue_game_manifest(nfshp "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(nfshp PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(nfshp PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(nfshp PRIVATE Vulkan::Headers)
endif()

# Guest memory + REX_GUEST_BARRIER, appended to each generated precompiled header.
foreach(t nfshp_recomp nfshp_game_module)
    if(TARGET ${t})
        target_precompile_headers(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/nfshp_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${t} PRIVATE -fasync-exceptions)
        endif()
    endif()
endforeach()
# macOS: the function registration table takes Apple clang ~forever at -O3; -O0 without the PCH builds in seconds.
if(APPLE)
    if(TARGET nfshp_recomp)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/nfshp_register.cpp"
                                    TARGET_DIRECTORY nfshp_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
    if(TARGET nfshp_game_module)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/game/nfshp_register.cpp"
                                    TARGET_DIRECTORY nfshp_game_module
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
