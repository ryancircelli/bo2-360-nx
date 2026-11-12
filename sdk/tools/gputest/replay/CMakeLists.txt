add_executable(rexgpu-replay replay_main.cpp)
target_link_libraries(rexgpu-replay PRIVATE rexruntime ${CMAKE_DL_LIBS})
