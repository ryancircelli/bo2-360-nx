# PPC instruction tests: asm/*.s from rexglue-sdk tests/ppc (BSD-3-Clause, rexglue 6384603).
# Each .s is assembled with the powerpc-none-elf binutils, recompiled by `rexglue recompile-tests`
# and checked as a Catch2 case against the REGISTER_OUT / MEMORY_OUT values in the file.
# REXGLUE_PPC_PREBUILT_BIN_DIR: use .bin/.map assembled elsewhere (the binutils are x86-64 Linux),
# e.g. to run the cases on an arm64 Mac.
set(REXGLUE_PPC_PREBUILT_BIN_DIR "" CACHE PATH "Prebuilt .bin/.map files for the PPC instruction tests")
# REXGLUE_PPC_ASM_DIR: run another set of .s cases in the same format, e.g. xenia-canary's.
set(REXGLUE_PPC_ASM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/asm CACHE PATH "PPC instruction test .s files")

set(PPC_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/generated)
file(MAKE_DIRECTORY ${PPC_GEN_DIR})
file(GLOB ASM_FILES ${REXGLUE_PPC_ASM_DIR}/*.s)

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
            --asm-dir=${REXGLUE_PPC_ASM_DIR}
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

# Known-failing cases (expected-fail.txt, one commented line each) run as WILL_FAIL so the default ctest set is green.
# Runs after catch_discover_tests' include file, which defines the tests only at ctest time.
set(PPC_XFAIL_LIST ${CMAKE_CURRENT_SOURCE_DIR}/expected-fail.txt)
set(PPC_XFAIL_CMAKE ${CMAKE_CURRENT_BINARY_DIR}/ppc_expected_fail.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PPC_XFAIL_LIST})
file(STRINGS ${PPC_XFAIL_LIST} PPC_XFAIL_LINES REGEX "^ppc\\.")
set(PPC_XFAIL_BODY "")
foreach(line ${PPC_XFAIL_LINES})
    string(REGEX REPLACE "[ \t]*#.*$" "" name "${line}")
    string(APPEND PPC_XFAIL_BODY "set_tests_properties(\"${name}\" PROPERTIES WILL_FAIL TRUE LABELS \"ppc;known-fail\")\n")
endforeach()
file(WRITE ${PPC_XFAIL_CMAKE} "${PPC_XFAIL_BODY}")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${PPC_XFAIL_CMAKE})
