# Unit Tests for ReXGlue core components

add_executable(codegen_branch_tests
    codegen/conditional_branch_test.cpp
)

target_include_directories(codegen_branch_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(codegen_branch_tests PRIVATE
    rexcodegen
    Catch2::Catch2WithMain
)

add_test(NAME codegen_branch_tests COMMAND codegen_branch_tests)

add_executable(chrono_steady_cast_tests
    chrono/chrono_steady_cast_test.cpp
)

target_include_directories(chrono_steady_cast_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(chrono_steady_cast_tests PRIVATE
    rex::core
    Catch2::Catch2WithMain
)

add_test(NAME chrono_steady_cast_tests COMMAND chrono_steady_cast_tests)

add_executable(slot_table_tests input/slot_table_test.cpp)
target_link_libraries(slot_table_tests PRIVATE rexruntime Catch2::Catch2WithMain)
add_test(NAME slot_table_tests COMMAND slot_table_tests)

add_executable(metal_constant_overrides_tests graphics/metal_constant_overrides_test.cpp)
target_include_directories(metal_constant_overrides_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/metal/port)
add_test(NAME metal_constant_overrides_tests COMMAND metal_constant_overrides_tests)

add_executable(patch_loader_tests patch/patch_loader_test.cpp)
target_include_directories(patch_loader_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(patch_loader_tests PRIVATE rexruntime)
add_library(patch_test_lib_v2 SHARED patch/patch_lib_v.cpp)
target_include_directories(patch_test_lib_v2 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(patch_test_lib_v2 PRIVATE PATCH_RET=2)
add_library(patch_test_lib_v3 SHARED patch/patch_lib_v.cpp)
target_include_directories(patch_test_lib_v3 PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(patch_test_lib_v3 PRIVATE PATCH_RET=3)
add_test(NAME patch_loader_tests COMMAND patch_loader_tests $<TARGET_FILE:patch_test_lib_v2> $<TARGET_FILE:patch_test_lib_v3> ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(patchable_all_tests patch/patchable_all_test.cpp)
    target_include_directories(patchable_all_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(patchable_all_tests PRIVATE rexruntime)
    add_test(NAME patchable_all_tests COMMAND patchable_all_tests)
endif()

add_executable(log_sink_tests logging/log_sink_test.cpp)
target_include_directories(log_sink_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(log_sink_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME log_sink_tests COMMAND log_sink_tests)

add_executable(fpscr_tests
    ppc/fpscr_test.cpp
)

target_include_directories(fpscr_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(fpscr_tests PRIVATE
    rex::core
    Catch2::Catch2WithMain
)

add_test(NAME fpscr_tests COMMAND fpscr_tests)

add_executable(xthread_tests
    system/xthread_test.cpp
)

target_include_directories(xthread_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(xthread_tests PRIVATE
    rexruntime
    Catch2::Catch2WithMain
)

set_target_properties(xthread_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)

add_test(NAME xthread_tests COMMAND xthread_tests)

add_executable(bench_clock_tests core/bench_clock_test.cpp)
target_include_directories(bench_clock_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bench_clock_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME bench_clock_tests COMMAND bench_clock_tests)

add_executable(sha512_selfcheck crypto/sha512_selfcheck.cpp)
target_include_directories(sha512_selfcheck PRIVATE ${PROJECT_SOURCE_DIR}/src/kernel/xboxkrnl)
add_test(NAME sha512_selfcheck COMMAND sha512_selfcheck)

add_executable(disc_swap_tests filesystem/disc_swap_test.cpp)
target_include_directories(disc_swap_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(disc_swap_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(disc_swap_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME disc_swap_tests COMMAND disc_swap_tests)
