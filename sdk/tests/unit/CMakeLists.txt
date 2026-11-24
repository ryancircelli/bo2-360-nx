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

add_executable(log_sink_tests logging/log_sink_test.cpp)
target_include_directories(log_sink_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(log_sink_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME log_sink_tests COMMAND log_sink_tests)

add_executable(bench_clock_tests core/bench_clock_test.cpp)
target_include_directories(bench_clock_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bench_clock_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME bench_clock_tests COMMAND bench_clock_tests)
