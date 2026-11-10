# rex-padtest: controller list, player slots and live state, without a game.
add_executable(rex-padtest padtest_main.cpp)
target_include_directories(rex-padtest PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(rex-padtest PRIVATE rexruntime)
set_target_properties(rex-padtest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
