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
