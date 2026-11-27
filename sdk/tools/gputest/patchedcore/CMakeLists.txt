# Linux build of the PATCHED shared graphics core: the exact tree the Metal plugin compiles on the Mac
# (include/rex/graphics + src/graphics with src/graphics/metal/patches applied), built without any
# Metal framework. Lets laptop-check compile, run and shader-sweep the code that ships, beside the
# stock core that rexgpu-xenos still builds. Not part of any game build (REXGLUE_BUILD_PATCHED_CORE).
#
# The private copy is staged at configure time with the same script and patch list as the plugin
# (src/graphics/metal/sdk_private.cmake, patches.cmake); laptop-check re-runs cmake on every check.
set(_metal_dir ${PROJECT_SOURCE_DIR}/src/graphics/metal)
include(${_metal_dir}/patches.cmake)
find_program(REXGPU_PATCH patch REQUIRED)
list(JOIN REXGPU_METAL_PATCHES "," _patch_arg)
set(SDK ${CMAKE_CURRENT_BINARY_DIR}/sdk-private)
file(GLOB_RECURSE _inputs CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/include/rex/graphics/* ${PROJECT_SOURCE_DIR}/src/graphics/*)
list(FILTER _inputs EXCLUDE REGEX "/src/graphics/metal/")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs} ${_metal_dir}/patches
    ${_metal_dir}/sdk_private.cmake ${_metal_dir}/patches.cmake)
execute_process(
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DMETAL_DIR=${_metal_dir} -DPRIVATE=${SDK}
            -DPATCH=${REXGPU_PATCH} -DPATCHES=${_patch_arg} -P ${_metal_dir}/sdk_private.cmake
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "patchedcore: staging the patched graphics sources failed")
endif()

include(${_metal_dir}/sources.cmake)   # REXGPU_METAL_COMMON_SOURCES (the shared core, ${SDK}/...)
set(_flags ${_metal_dir}/compat.h)
set(_inc BEFORE PRIVATE ${SDK}/include ${SDK} ${_metal_dir}/port ${_metal_dir}
    ${PROJECT_SOURCE_DIR}/thirdparty/renderdoc ${PROJECT_SOURCE_DIR})
set(_opts -fno-strict-aliasing -ffp-model=strict -fno-char8_t -Wno-unused-variable)

# 1. The shared core alone, with only the platform-neutral compat layer: proves it needs nothing Metal.
add_library(rexgpu-patched-core STATIC ${REXGPU_METAL_COMMON_SOURCES})
target_include_directories(rexgpu-patched-core ${_inc})
target_compile_options(rexgpu-patched-core PRIVATE -include ${PROJECT_SOURCE_DIR}/include/rex/graphics/xenia_compat.h ${_opts})
target_link_libraries(rexgpu-patched-core PUBLIC rexruntime xxHash::xxhash)

# 2. The Metal shader path without Metal: Xenos -> DXBC (bindings) -> MSL text, over microcode dumps.
add_executable(rexgpu-patched-mslsweep mslsweep.cpp ${_metal_dir}/metal_flags.cpp
    ${_metal_dir}/port/rex/graphics/metal/metal_msl_translator.cc)
target_include_directories(rexgpu-patched-mslsweep ${_inc})
target_compile_options(rexgpu-patched-mslsweep PRIVATE -include ${_flags} ${_opts})
target_link_libraries(rexgpu-patched-mslsweep PRIVATE rexgpu-patched-core)

# 3. The plugin's own translator self-test.
add_executable(rexgpu-patched-msltest ${_metal_dir}/metal_flags.cpp
    ${_metal_dir}/port/rex/graphics/metal/metal_msl_translator.cc ${_metal_dir}/tools/msl_translator_test.cc)
target_include_directories(rexgpu-patched-msltest ${_inc})
target_compile_options(rexgpu-patched-msltest PRIVATE -include ${_flags} ${_opts})
target_link_libraries(rexgpu-patched-msltest PRIVATE rexgpu-patched-core)
add_test(NAME patched_msl_translator_test COMMAND rexgpu-patched-msltest ${CMAKE_CURRENT_BINARY_DIR}/msltest-out)
