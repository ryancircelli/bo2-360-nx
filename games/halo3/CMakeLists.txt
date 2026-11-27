cmake_minimum_required(VERSION 3.24)
project(halo3 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# Generate all images and run the shared post-passes before building the title.
# Module targets depend on this marker when host-tool generation is disabled.
if(NOT REXGLUE_BUILD_HOST_TOOLS AND NOT TARGET halo3_codegen)
    add_custom_target(halo3_codegen)
endif()
# Bootstrap the current SDK code generator in the same build directory used by the game.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
    return()
endif()
include(generated/rexglue.cmake)

add_executable(halo3 src/main.cpp)
rexglue_setup_target(halo3 GPU_PLUGINS metal xenos)
rexglue_game_manifest(halo3 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(TARGET imgui)
    target_include_directories(halo3 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET renderdoc)
    target_include_directories(halo3 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(halo3 PRIVATE Vulkan::Headers)
endif()

# Preserve the shared guest-memory barriers and asynchronous guest SEH behavior.
function(halo3_configure_guest target directory)
    if(TARGET ${target})
        target_precompile_headers(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/halo3_guest_memory.h")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -fasync-exceptions)
        endif()
        # Registration tables need no optimization; avoid excessive Apple clang compile time.
        if(APPLE)
            set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/${directory}/halo3_register.cpp"
                TARGET_DIRECTORY ${target}
                PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
        endif()
    endif()
endfunction()
halo3_configure_guest(halo3_recomp default)
halo3_configure_guest(halo3_WaveShell-Xbox waveshell)
halo3_configure_guest(halo3_WavesLibDLL waveslib)
halo3_configure_guest(halo3_L360 l360)
halo3_configure_guest(halo3_Q10 q10)
