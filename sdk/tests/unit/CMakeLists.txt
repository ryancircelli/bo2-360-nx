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

add_executable(codegen_vector_tests
    codegen/vector_test.cpp
)

target_include_directories(codegen_vector_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/thirdparty/simde
)

target_link_libraries(codegen_vector_tests PRIVATE
    rexcodegen
    Catch2::Catch2WithMain
)

add_test(NAME codegen_vector_tests COMMAND codegen_vector_tests)

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
