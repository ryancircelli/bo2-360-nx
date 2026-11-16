#!/bin/bash
set -e
cd /Users/ryanc/dev/bo2-360-nx-wt/nfsmw-saves-movies/games/nfsmw
cmake -S . -B out/mac-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DREXGLUE_BUILD_HOST_TOOLS=OFF -DREXGLUE_USE_METAL=ON -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=ON -DREXGLUE_ENABLE_PERF_COUNTERS=ON -DREXGLUE_ENABLE_FIDELITYFX=OFF -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_BUILD_PADTEST=OFF -DREXSDK_DIR=/Users/ryanc/dev/bo2-360-nx-wt/nfsmw-saves-movies/games/nfsmw/../../sdk
~/Games/Xbox360/bo2-build/runq/bin/buildq -- cmake --build out/mac-trace
