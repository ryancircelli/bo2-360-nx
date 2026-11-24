# The translator lives in the rexgpu-xenos plugin, which hides its symbols, so compile the translate-only sources directly.
set(G ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/graphics)
add_executable(rexgpu-shadersweep shadersweep.cpp
    ${G}/flags.cpp ${G}/register_file.cpp ${G}/registers.cpp ${G}/format/ucode.cpp ${G}/util/draw.cpp
    ${G}/pipeline/shader/shader.cpp ${G}/pipeline/shader/translator.cpp ${G}/pipeline/shader/translator_disasm.cpp
    ${G}/pipeline/shader/interpreter.cpp ${G}/pipeline/shader/spirv.cpp ${G}/pipeline/shader/spirv_builder.cpp
    ${G}/pipeline/shader/spirv_translator.cpp ${G}/pipeline/shader/spirv_translator_alu.cpp
    ${G}/pipeline/shader/spirv_translator_fetch.cpp ${G}/pipeline/shader/spirv_translator_memexport.cpp
    ${G}/pipeline/shader/spirv_translator_rb.cpp)
target_link_libraries(rexgpu-shadersweep PRIVATE rexruntime xxHash::xxhash Vulkan::Headers renderdoc
    glslang::SPIRV glslang MachineIndependent GenericCodeGen OSDependent OGLCompiler)
