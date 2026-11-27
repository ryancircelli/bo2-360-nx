# Gears of War (Xbox 360, 2006), single executable.
cmake_minimum_required(VERSION 3.24)
project(gears CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
# Generated code is prepared before configuring the game build.
include(generated/rexglue.cmake)
# Cross builds get no codegen target from rexglue.cmake; keep the name dependable.
if(NOT TARGET gears_codegen)
    add_custom_target(gears_codegen)
endif()

add_executable(gears)
rexglue_setup_target(gears GPU_PLUGINS metal xenos)
rexglue_game_manifest(gears "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

rexglue_game_defaults(gears MAIN)
