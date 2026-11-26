# Gears of War (Xbox 360, 2006), single executable.
cmake_minimum_required(VERSION 3.24)
project(gears CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# Generated code is prepared before configuring the game build.
if(NOT TARGET gears_codegen)
    add_custom_target(gears_codegen)
endif()
include(generated/rexglue.cmake)

add_executable(gears src/main.cpp)
rexglue_setup_target(gears GPU_PLUGINS metal xenos)
rexglue_game_manifest(gears "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(gears PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(gears PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(gears PRIVATE Vulkan::Headers)
endif()

# Guest memory + REX_GUEST_BARRIER, appended to each generated precompiled header.
foreach(t gears_recomp)
    if(TARGET ${t})
        target_precompile_headers(${t} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gears_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${t} PRIVATE -fasync-exceptions)
        endif()
    endif()
endforeach()
# macOS: the function registration table takes Apple clang ~forever at -O3; -O0 without the PCH builds in seconds.
if(APPLE)
    if(TARGET gears_recomp)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/gears_register.cpp"
                                    TARGET_DIRECTORY gears_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
