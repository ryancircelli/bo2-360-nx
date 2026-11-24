add_executable(rexgpu-shadersweep shadersweep.cpp)
target_link_libraries(rexgpu-shadersweep PRIVATE rexruntime rexgpu-xenos glslang::SPIRV Vulkan::Headers renderdoc)
