# Host tool: bo2zm_spv2msl (the shader pack to MSL ahead of time, see bo2zm_spv2msl.cpp). Built on its own:
#   cmake -S tools/metal -B <build> -G Ninja && cmake --build <build>
# Uses an installed SPIRV-Cross (Homebrew spirv-cross, or any CMake package of it); with
# -DBO2ZM_FETCH_SPIRV_CROSS=ON it fetches the pinned SPIRV-Cross instead.
cmake_minimum_required(VERSION 3.20)
project(bo2zm_metal_tools CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BO2ZM_FETCH_SPIRV_CROSS "Fetch SPIRV-Cross instead of using an installed one" OFF)
if(BO2ZM_FETCH_SPIRV_CROSS)
  include(FetchContent)
  set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG vulkan-sdk-1.4.363.0)  # the version Homebrew ships today
  FetchContent_MakeAvailable(spirv_cross)
else()
  find_package(spirv_cross_core CONFIG REQUIRED)
  find_package(spirv_cross_glsl CONFIG REQUIRED)
  find_package(spirv_cross_msl CONFIG REQUIRED)
endif()

add_executable(bo2zm_spv2msl bo2zm_spv2msl.cpp)
target_link_libraries(bo2zm_spv2msl PRIVATE spirv-cross-msl spirv-cross-glsl spirv-cross-core)
