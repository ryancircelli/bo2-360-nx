# Unit Tests for ReXGlue core components

add_executable(codegen_branch_tests
    codegen/conditional_branch_test.cpp
    codegen/jump_table_bounds_test.cpp
    codegen/stack_access_test.cpp
    codegen/function_boundary_test.cpp
    codegen/jump_thunk_millicode_test.cpp
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

add_executable(thread_spin_tests core/thread_spin_test.cpp)
target_include_directories(thread_spin_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(thread_spin_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME thread_spin_tests COMMAND thread_spin_tests)

add_executable(slot_table_tests input/slot_table_test.cpp)
target_link_libraries(slot_table_tests PRIVATE rexruntime Catch2::Catch2WithMain)
add_test(NAME slot_table_tests COMMAND slot_table_tests)

add_executable(guide_input_gate_tests ui/guide_input_gate_test.cpp)
target_link_libraries(guide_input_gate_tests PRIVATE rexruntime imgui::imgui Catch2::Catch2WithMain)
add_test(NAME guide_input_gate_tests COMMAND guide_input_gate_tests)

add_executable(host_path_rename_tests filesystem/host_path_rename_test.cpp)
target_link_libraries(host_path_rename_tests PRIVATE rexruntime Catch2::Catch2WithMain)
add_test(NAME host_path_rename_tests COMMAND host_path_rename_tests)

add_executable(live_pad_tests input/live_pad_test.cpp)
target_link_libraries(live_pad_tests PRIVATE rexruntime Catch2::Catch2WithMain)
add_test(NAME live_pad_tests COMMAND live_pad_tests)

add_executable(metal_constant_overrides_tests graphics/metal_constant_overrides_test.cpp)
target_include_directories(metal_constant_overrides_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/metal/port)
add_test(NAME metal_constant_overrides_tests COMMAND metal_constant_overrides_tests)

add_executable(metal_readback_merge_tests graphics/metal_readback_merge_test.cpp)
target_include_directories(metal_readback_merge_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/graphics/metal/port)
add_test(NAME metal_readback_merge_tests COMMAND metal_readback_merge_tests)

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

add_executable(clock_rate_tests chrono/clock_rate_test.cpp)
target_include_directories(clock_rate_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(clock_rate_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME clock_rate_tests COMMAND clock_rate_tests)

add_executable(sha512_selfcheck crypto/sha512_selfcheck.cpp)
target_include_directories(sha512_selfcheck PRIVATE ${PROJECT_SOURCE_DIR}/src/kernel/xboxkrnl)
add_test(NAME sha512_selfcheck COMMAND sha512_selfcheck)

add_executable(xmemory_heap_tests system/xmemory_heap_test.cpp)
target_link_libraries(xmemory_heap_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(xmemory_heap_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
add_test(NAME xmemory_heap_tests COMMAND xmemory_heap_tests)

add_executable(disc_swap_tests filesystem/disc_swap_test.cpp)
target_include_directories(disc_swap_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(disc_swap_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(disc_swap_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME disc_swap_tests COMMAND disc_swap_tests)

add_executable(keyboard_answer_tests kernel/keyboard_answer_test.cpp)
target_link_libraries(keyboard_answer_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(keyboard_answer_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
add_test(NAME keyboard_answer_tests COMMAND keyboard_answer_tests)

add_executable(rsa_public_selfcheck crypto/rsa_public_test.cpp)
target_include_directories(rsa_public_selfcheck PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME rsa_public_selfcheck COMMAND rsa_public_selfcheck)

add_executable(xma_tests audio/xma_test.cpp)
target_include_directories(xma_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(xma_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(xma_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME xma_tests COMMAND xma_tests)

add_executable(guest_stack_range_tests runtime_stack_range_test.cpp)
target_include_directories(guest_stack_range_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(guest_stack_range_tests PRIVATE rex::core Catch2::Catch2WithMain)
add_test(NAME guest_stack_range_tests COMMAND guest_stack_range_tests)

add_executable(xevent_tests system/xevent_test.cpp)
target_include_directories(xevent_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(xevent_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(xevent_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME xevent_tests COMMAND xevent_tests)

add_executable(runtime_objects_tests system/runtime_objects_test.cpp)
target_include_directories(runtime_objects_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(runtime_objects_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(runtime_objects_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME runtime_objects_tests COMMAND runtime_objects_tests)
set_tests_properties(runtime_objects_tests PROPERTIES TIMEOUT 30)

add_executable(vfs_devices_tests filesystem/devices_test.cpp)
target_include_directories(vfs_devices_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(vfs_devices_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(vfs_devices_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME vfs_devices_tests COMMAND vfs_devices_tests)

add_executable(content_manager_tests system/content_manager_test.cpp)
target_include_directories(content_manager_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(content_manager_tests PRIVATE rexruntime Catch2::Catch2WithMain)
set_target_properties(content_manager_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
add_test(NAME content_manager_tests COMMAND content_manager_tests)

add_executable(codegen_xdk_signature_tests codegen/xdk_signatures_test.cpp)
target_include_directories(codegen_xdk_signature_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(codegen_xdk_signature_tests PRIVATE rexcodegen Catch2::Catch2WithMain)
add_test(NAME codegen_xdk_signature_tests COMMAND codegen_xdk_signature_tests)
