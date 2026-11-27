# The Metal plugin's native MSL translator on any host: its private patched SDK copy (the two patches that touch
# the shader translators, a prefix of the plugin's patch order) plus the translate-only sources.
set(M ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/graphics/metal)
set(P ${CMAKE_CURRENT_BINARY_DIR}/sdk-private)
find_program(MSLSWEEP_PATCH patch REQUIRED)
execute_process(COMMAND ${CMAKE_COMMAND} -DROOT=${REXGLUE_ROOT} -DMETAL_DIR=${M} -DPRIVATE=${P}
    -DPATCH=${MSLSWEEP_PATCH} -DPATCHES=sdk-metal.patch,sdk-metal-native-scale.patch -P ${M}/sdk_private.cmake
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "rexgpu-mslsweep: preparing the patched graphics sources failed")
endif()
set(S ${P}/src/graphics)
add_executable(rexgpu-mslsweep mslsweep.cpp
    ${S}/flags.cpp ${S}/register_file.cpp ${S}/registers.cpp ${S}/format/ucode.cpp ${S}/util/draw.cpp
    ${S}/pipeline/shader/shader.cpp ${S}/pipeline/shader/translator.cpp ${S}/pipeline/shader/translator_disasm.cpp
    ${S}/pipeline/shader/dxbc.cpp ${S}/pipeline/shader/dxbc_translator.cpp ${S}/pipeline/shader/dxbc_translator_alu.cpp
    ${S}/pipeline/shader/dxbc_translator_fetch.cpp ${S}/pipeline/shader/dxbc_translator_memexport.cpp
    ${S}/pipeline/shader/dxbc_translator_om.cpp ${P}/thirdparty/dxbc/DXBCChecksum.cpp
    ${M}/metal_flags.cpp ${M}/port/rex/graphics/metal/metal_msl_translator.cc)
target_include_directories(rexgpu-mslsweep BEFORE PRIVATE ${M}/port ${P}/include ${P} ${M})
target_compile_options(rexgpu-mslsweep PRIVATE -include ${M}/compat.h -fno-strict-aliasing -fno-char8_t)
target_link_libraries(rexgpu-mslsweep PRIVATE rexruntime xxHash::xxhash renderdoc)
