# PPC instruction tests: asm/*.s from rexglue-sdk tests/ppc (BSD-3-Clause, rexglue 6384603).
# Each .s is assembled with the powerpc-none-elf binutils, recompiled by `rexglue recompile-tests`
# and checked as a Catch2 case against the REGISTER_OUT / MEMORY_OUT values in the file.
# REXGLUE_PPC_PREBUILT_BIN_DIR: use .bin/.map assembled elsewhere (the binutils are x86-64 Linux),
# e.g. to run the cases on an arm64 Mac.
set(REXGLUE_PPC_PREBUILT_BIN_DIR "" CACHE PATH "Prebuilt .bin/.map files for the PPC instruction tests")

set(PPC_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/generated)
file(MAKE_DIRECTORY ${PPC_GEN_DIR})
file(GLOB ASM_FILES ${CMAKE_CURRENT_SOURCE_DIR}/asm/*.s)

if(REXGLUE_PPC_PREBUILT_BIN_DIR)
    set(PPC_BIN_DIR ${REXGLUE_PPC_PREBUILT_BIN_DIR})
    set(TEST_BINS "")
else()
    include(ppc_test_pipeline)
    set(PPC_OBJ_DIR ${CMAKE_BINARY_DIR}/tests/ppc/obj)
    set(PPC_BIN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/bin)
    file(MAKE_DIRECTORY ${PPC_OBJ_DIR} ${PPC_BIN_DIR})
    foreach(asm_file ${ASM_FILES})
        ppc_add_test_binary(${asm_file} ${PPC_OBJ_DIR} ${PPC_BIN_DIR})
    endforeach()
    get_property(TEST_BINS GLOBAL PROPERTY PPC_TEST_BINS)
    get_property(TEST_MAPS GLOBAL PROPERTY PPC_TEST_MAPS)
    list(APPEND TEST_BINS ${TEST_MAPS})
endif()

set(PPC_GENERATED
    ${PPC_GEN_DIR}/ppc_test_functions.cpp
    ${PPC_GEN_DIR}/ppc_test_cases.cpp
    ${PPC_GEN_DIR}/ppc_test_decls.h
    ${PPC_GEN_DIR}/ppc_config.h
)
add_custom_command(
    OUTPUT ${PPC_GENERATED}
    COMMAND $<TARGET_FILE:rexglue> recompile-tests --output=${PPC_GEN_DIR} --bin-dir=${PPC_BIN_DIR}
            --asm-dir=${CMAKE_CURRENT_SOURCE_DIR}/asm
    DEPENDS ${TEST_BINS} ${ASM_FILES} rexglue
    COMMENT "Generating PPC instruction tests with rexglue recompile-tests"
    VERBATIM
)

add_executable(ppc_tests ${PPC_GEN_DIR}/ppc_test_functions.cpp ${PPC_GEN_DIR}/ppc_test_cases.cpp)
target_include_directories(ppc_tests PRIVATE ${PPC_GEN_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ppc_tests PRIVATE rexruntime Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(ppc_tests TEST_PREFIX "ppc." PROPERTIES LABELS "ppc" TIMEOUT 60)

# vmaddfp/vnmsubfp are fused: an x86 host needs FMA for simde to fuse them (arm64 always does)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(ppc_tests PRIVATE -mfma)
endif()
