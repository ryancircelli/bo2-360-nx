cmake_minimum_required(VERSION 3.24)
project(bo1 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")
include(generated/rexglue.cmake)

add_executable(bo1 src/diagnostics.cpp)
rexglue_setup_target(bo1 GPU_PLUGINS metal xenos)
rexglue_game_manifest(bo1 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")

rexglue_game_defaults(bo1 MAIN)
