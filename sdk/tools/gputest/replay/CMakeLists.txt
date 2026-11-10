# rexgpu-replay: offline GPU trace replay with per-draw readback.
# Built when REXGLUE_BUILD_GPUTEST=ON; the GPU plugin then also carries the
# capture hooks and the rex_gpu_trace_replay export.

add_executable(rexgpu-replay replay_main.cpp)
target_link_libraries(rexgpu-replay PRIVATE rexruntime ${CMAKE_DL_LIBS})
if(TARGET rexgpu-xenos)
    add_dependencies(rexgpu-replay rexgpu-xenos)
    # Stage the plugin next to the tool, as the loader expects.
    add_custom_command(TARGET rexgpu-replay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rexgpu-xenos> $<TARGET_FILE_DIR:rexgpu-replay>)
endif()
