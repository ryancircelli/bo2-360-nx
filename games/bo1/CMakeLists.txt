cmake_minimum_required(VERSION 3.24)
project(bo1 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
include(generated/rexglue.cmake)

add_executable(bo1 src/main.cpp src/diagnostics.cpp)
rexglue_setup_target(bo1 GPU_PLUGINS metal xenos)
rexglue_game_manifest(bo1 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(bo1 PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(bo1 PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET Vulkan::Headers)
    target_link_libraries(bo1 PRIVATE Vulkan::Headers)
endif()

if(TARGET bo1_recomp)
    target_precompile_headers(bo1_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bo1_guest_memory.h")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(bo1_recomp PRIVATE -fasync-exceptions)
    endif()
    if(APPLE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default/bo1_register.cpp"
                                    TARGET_DIRECTORY bo1_recomp
                                    PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()
