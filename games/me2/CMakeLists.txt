# Mass Effect 2 (Xbox 360, 2010), two-disc title.
cmake_minimum_required(VERSION 3.24)
project(me2 CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" CACHE PATH "Path to the rexglue-sdk source tree")

include(generated/rexglue.cmake)

add_executable(me2)
rexglue_setup_target(me2 GPU_PLUGINS metal xenos)
rexglue_game_manifest(me2 "${CMAKE_CURRENT_SOURCE_DIR}/game.toml")
rexglue_game_defaults(me2 MAIN)
