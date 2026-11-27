# Mass Effect 2 (Xbox 360, 2010), two-disc title.
cmake_minimum_required(VERSION 3.24)
project(me2 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# Generated code is prepared before configuring the game build.
if(NOT TARGET me2_codegen)
    add_custom_target(me2_codegen)
endif()
include(generated/rexglue.cmake)

add_executable(me2 src/main.cpp)
rexglue_setup_target(me2 GPU_PLUGINS metal xenos)
rexglue_game_manifest(me2 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(me2 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(me2 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(me2 PRIVATE Vulkan::Headers)
endif()

# Guest memory + REX_GUEST_BARRIER, appended to each generated precompiled header.
foreach(t me2_recomp)
    if(TARGET ${t})
        target_precompile_headers(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/me2_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${t} PRIVATE -fasync-exceptions)
        endif()
    endif()
endforeach()
# macOS: the function registration table takes Apple clang ~forever at -O3; -O0 without the PCH builds in seconds.
if(APPLE)
    if(TARGET me2_recomp)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/me2_register.cpp"
                                    TARGET_DIRECTORY me2_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
