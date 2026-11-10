# rexgpu-metal: native Metal Xenos GPU emulation plugin (macOS, Apple Silicon)
#
# Adapted from the Fable II Recomp for macOS Metal backend (MIT), which adapts
# XeniOS's Metal backend (BSD-3-Clause) to the ReXGlue GPU plugin ABI. See
# README.md and provenance.json in this directory.
#
# Runtime-loaded like rexgpu-xenos: set the gpu_plugin cvar to "metal" (or
# pass GPU_PLUGINS metal to rexglue_configure_target to stage it).
#
# Build shape:
#   1. Pinned, hash-checked downloads (dependencies.json): metal-cpp, the Metal
#      Shader Converter runtime headers, and XeniOS (helper shader sources).
#      No DXIL converter and no Metal Shader Converter library: this is the
#      "no-converter" configuration, guest shaders go through the native
#      Xenos -> MSL translator only.
#   2. A private copy of the SDK graphics sources with patches/*.patch applied.
#      Only this plugin compiles it; rexgpu-xenos and the runtime keep the
#      unpatched sources.
#   3. Built-in helper shaders compiled to metallib headers with the Xcode
#      Metal toolchain (xcodebuild -downloadComponent MetalToolchain).

if(NOT APPLE OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(FATAL_ERROR "REXGLUE_USE_METAL requires macOS on Apple Silicon")
endif()

enable_language(OBJCXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(REXGPU_METAL_PATCH patch REQUIRED)
include(FetchContent)

set(_metal_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(_metal_work ${CMAKE_CURRENT_BINARY_DIR})
set(_metal_deps ${_metal_work}/deps)
set(_metal_gen ${_metal_work}/generated)

# ---------------------------------------------------------------------------
# 1. Pinned dependencies
# ---------------------------------------------------------------------------
file(READ ${_metal_dir}/dependencies.json _metal_pins)
string(JSON _metal_pin_count LENGTH "${_metal_pins}")
math(EXPR _metal_pin_last "${_metal_pin_count} - 1")
foreach(_i RANGE ${_metal_pin_last})
    string(JSON _name GET "${_metal_pins}" ${_i} name)
    string(JSON _url GET "${_metal_pins}" ${_i} url)
    string(JSON _sha GET "${_metal_pins}" ${_i} sha256)
    string(MAKE_C_IDENTIFIER "rexgpu_metal_${_name}" _fc_name)
    # SOURCE_SUBDIR points at a directory that does not exist so that
    # MakeAvailable only populates (no add_subdirectory of upstream builds).
    FetchContent_Declare(${_fc_name}
        URL ${_url}
        URL_HASH SHA256=${_sha}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR ${_metal_deps}/${_name}
        SOURCE_SUBDIR rexglue-populate-only
    )
    FetchContent_MakeAvailable(${_fc_name})
endforeach()

set(_mcpp ${_metal_deps}/metal-cpp)
set(_msc ${_metal_deps}/metal-shader-converter)
set(_xenios ${_metal_deps}/xenios-source)

# The adapted sources include "third_party/metal-cpp/..." and
# "third_party/metal-shader-converter/...".
file(MAKE_DIRECTORY ${_metal_deps}/third_party)
foreach(_name metal-cpp metal-shader-converter)
    if(NOT EXISTS ${_metal_deps}/third_party/${_name})
        file(CREATE_LINK ../${_name} ${_metal_deps}/third_party/${_name} SYMBOLIC)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# 2. Private, patched copy of the SDK graphics sources
# ---------------------------------------------------------------------------
set(_metal_patches
    sdk-metal.patch
    sdk-metal-native-scale.patch
    sdk-metal-rt-shrink.patch
    sdk-metal-hle-census.patch
    sdk-metal-gpucmd.patch
    sdk-metal-ssmask.patch
)
set(_sdk_private ${_metal_work}/sdk-private)

file(GLOB_RECURSE _sdk_graphics_inputs CONFIGURE_DEPENDS
    ${REXGLUE_ROOT}/include/rex/graphics/*
    ${REXGLUE_ROOT}/src/graphics/*.cpp
    ${REXGLUE_ROOT}/src/graphics/*.h
    ${REXGLUE_ROOT}/thirdparty/dxbc/*)
list(FILTER _sdk_graphics_inputs EXCLUDE REGEX "/src/graphics/metal/")
set(_sdk_fingerprint "")
foreach(_f IN LISTS _sdk_graphics_inputs)
    file(SHA256 ${_f} _h)
    string(APPEND _sdk_fingerprint "${_f}:${_h}\n")
endforeach()
foreach(_p IN LISTS _metal_patches)
    file(SHA256 ${_metal_dir}/patches/${_p} _h)
    string(APPEND _sdk_fingerprint "${_p}:${_h}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_metal_dir}/patches/${_p})
endforeach()
string(SHA256 _sdk_fingerprint "${_sdk_fingerprint}")

set(_sdk_stamp ${_sdk_private}/.rexgpu-metal-fingerprint)
set(_sdk_old "")
if(EXISTS ${_sdk_stamp})
    file(READ ${_sdk_stamp} _sdk_old)
endif()
if(NOT _sdk_old STREQUAL _sdk_fingerprint)
    message(STATUS "rexgpu-metal: preparing patched graphics sources")
    file(REMOVE_RECURSE ${_sdk_private})
    file(MAKE_DIRECTORY ${_sdk_private}/include/rex ${_sdk_private}/src ${_sdk_private}/thirdparty)
    file(COPY ${REXGLUE_ROOT}/include/rex/graphics DESTINATION ${_sdk_private}/include/rex)
    file(COPY ${REXGLUE_ROOT}/src/graphics DESTINATION ${_sdk_private}/src
        REGEX "/src/graphics/metal$" EXCLUDE)
    file(COPY ${REXGLUE_ROOT}/thirdparty/dxbc DESTINATION ${_sdk_private}/thirdparty)
    foreach(_p IN LISTS _metal_patches)
        execute_process(
            COMMAND ${REXGPU_METAL_PATCH} --batch --no-backup-if-mismatch -p1
                    -i ${_metal_dir}/patches/${_p}
            WORKING_DIRECTORY ${_sdk_private}
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _out)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "rexgpu-metal: ${_p} does not apply to the SDK graphics sources:\n${_out}")
        endif()
    endforeach()
    file(WRITE ${_sdk_stamp} "${_sdk_fingerprint}")
endif()

# ---------------------------------------------------------------------------
# 3. Built-in helper shaders (metallib headers)
# ---------------------------------------------------------------------------
file(GLOB _metal_helper_wrappers CONFIGURE_DEPENDS ${_metal_dir}/shaders/*)
set(_metal_helpers_stamp ${_metal_gen}/helpers.stamp)
add_custom_command(
    OUTPUT ${_metal_helpers_stamp}
    COMMAND ${Python3_EXECUTABLE} -I ${_metal_dir}/tools/compile_metal_helpers.py
            ${_xenios} ${_metal_gen} 8
    COMMAND ${CMAKE_COMMAND} -E touch ${_metal_helpers_stamp}
    DEPENDS ${_metal_dir}/tools/compile_metal_helpers.py ${_metal_helper_wrappers}
    COMMENT "rexgpu-metal: compiling built-in Metal helper shaders"
    VERBATIM
)
add_custom_target(rexgpu-metal-helpers DEPENDS ${_metal_helpers_stamp})

# ---------------------------------------------------------------------------
# 4. The plugin
# ---------------------------------------------------------------------------
set(SDK ${_sdk_private})
include(${_metal_dir}/sources.cmake)

add_library(rexgpu-metal SHARED
    ${REXGPU_METAL_COMMON_SOURCES}
    ${REXGPU_METAL_SOURCES}
    plugin_main.cpp
    metal_flags.cpp
)
add_library(rex::gpu-metal ALIAS rexgpu-metal)
add_dependencies(rexgpu-metal rexgpu-metal-helpers)

set_target_properties(rexgpu-metal PROPERTIES
    EXPORT_NAME gpu-metal
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path"
)
if(COMMAND rexglue_add_version_resource)
    rexglue_add_version_resource(rexgpu-metal DESCRIPTION "ReXGlue Xenos GPU emulation (Metal)")
endif()

# Private patched SDK headers must come before the SDK's own include
# directory, which arrives through rexruntime's interface.
target_include_directories(rexgpu-metal BEFORE PRIVATE
    ${_metal_dir}/port
    ${_metal_gen}
    ${_sdk_private}/include
    ${_sdk_private}
    ${_metal_dir}
    ${_metal_deps}
    ${_msc}/include
    ${_msc}/include/metal_irconverter
    ${_msc}/include/metal_irconverter_runtime
)

target_compile_definitions(rexgpu-metal PRIVATE
    REX_PLATFORM_MAC=1
    METAL_SHADER_CONVERTER_AVAILABLE=1
    IR_RUNTIME_METALCPP=1
    REXGLUE_BUILD_CONFIG="$<CONFIG>"
    FABLE2_NO_CONVERTER=1
)

target_compile_options(rexgpu-metal PRIVATE
    -include ${_metal_dir}/compat.h
    -fno-strict-aliasing
    -ffp-model=strict
    -fno-char8_t
    -fblocks
    -Wno-unused-variable
)

# Everything PRIVATE, as for rexgpu-xenos: never link the OBJECT libraries.
target_link_libraries(rexgpu-metal PRIVATE
    rexruntime
    xxHash::xxhash
    z
    "-framework Metal"
    "-framework Foundation"
    "-framework AppKit"
    "-framework QuartzCore"
    "-framework MetalKit"
    "-framework MetalFX"
    "-framework ImageIO"
    "-framework CoreGraphics"
)

# Offline Xenos -> MSL translator driver (diagnostics; not built by default).
add_executable(rexgpu-metal-msl-xlate EXCLUDE_FROM_ALL
    ${REXGPU_METAL_COMMON_SOURCES}
    metal_flags.cpp
    port/rex/graphics/metal/metal_msl_translator.cc
    tools/msl_xlate_tool.cc
)
add_dependencies(rexgpu-metal-msl-xlate rexgpu-metal-helpers)
target_include_directories(rexgpu-metal-msl-xlate BEFORE PRIVATE
    ${_metal_dir}/port ${_metal_gen} ${_sdk_private}/include ${_sdk_private} ${_metal_dir} ${_metal_deps})
target_compile_definitions(rexgpu-metal-msl-xlate PRIVATE REX_PLATFORM_MAC=1)
target_compile_options(rexgpu-metal-msl-xlate PRIVATE
    -include ${_metal_dir}/compat.h -fno-strict-aliasing -ffp-model=strict -fno-char8_t)
target_link_libraries(rexgpu-metal-msl-xlate PRIVATE rexruntime xxHash::xxhash)
