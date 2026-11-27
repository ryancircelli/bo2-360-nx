# bo2cp - ReXGlue Recompiled Project
#
# Campaign bring-up: the TU18 default.xex recompiled on its own, drawn by the emulated GPU (xenos), for Windows.
# SDK boilerplate lives in generated/rexglue.cmake; build_windows.sh configures and builds it.

cmake_minimum_required(VERSION 3.25)
project(bo2cp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BO2CP_SOURCES
    src/main.cpp
    src/bo2cp_relaunch.cpp
    src/bo2cp_autoexec.cpp
    ../games/bo2/src/bo2zm_launch_options.cpp
)

# The native renderer (B3): the Zombies program's renderer and its D3D marker, built from games/bo2/src and core unchanged, with the
# campaign's addresses forced in (src/native/bo2cp_game_remap.h) and the Zombies-only systems it calls replaced by
# src/native/bo2cp_native_stubs.cpp. Off: the emulated GPU only.
option(BO2CP_NATIVE "Build the native renderer into bo2cp" ON)
set(BO2ZM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../games/bo2/src")
set(BO2ZM_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../core")
if(BO2CP_NATIVE)
    set(BO2CP_NATIVE_SOURCES
        ${BO2ZM_SRC}/native/render/bo2zm_native_render.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_targets.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_draws.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_deferred.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_shaders.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_shader_library.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_shader_index.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_progress.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_hooks.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_texture_pool.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_precompile.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_native_capture.cpp
        ${BO2ZM_SRC}/native/render/bo2zm_graphics_settings.cpp
        ${BO2ZM_SRC}/native/bo2zm_native_system.cpp
        ${BO2ZM_SRC}/native/bo2zm_render_stubs.cpp
        ${BO2ZM_SRC}/bo2zm_d3d_marker.cpp
        ${BO2ZM_CORE_SRC}/bo2zm_page_writes.cpp
        ${BO2ZM_CORE_SRC}/bo2zm_pc_profiler.cpp
        ${BO2ZM_SRC}/bo2zm_test_pad.cpp
        ${BO2ZM_SRC}/bo2zm_preset.cpp
        ${BO2ZM_CORE_SRC}/bo2zm_settings.cpp
        ${BO2ZM_SRC}/bo2zm_dvars.cpp
        ${BO2ZM_SRC}/bo2zm_controllers.cpp
        ${BO2ZM_SRC}/bo2zm_pad.cpp
        ${BO2ZM_SRC}/bo2zm_ui.cpp
        ${BO2ZM_SRC}/bo2zm_commands.cpp
        ${BO2ZM_SRC}/bo2zm_extras.cpp
        ${BO2ZM_SRC}/bo2zm_script_manager.cpp
        ${BO2ZM_SRC}/bo2zm_bots.cpp
        ${BO2ZM_SRC}/bo2zm_name.cpp
        ${BO2ZM_SRC}/bo2zm_debug_options.cpp
        ${BO2ZM_SRC}/bo2zm_switch.cpp
        ${BO2ZM_SRC}/bo2zm_webm.cpp
        ${BO2ZM_SRC}/bo2zm_sound_mix.cpp
        ${BO2ZM_SRC}/bo2zm_crt.cpp
        ${BO2ZM_SRC}/bo2zm_native_cull.cpp
        ${BO2ZM_SRC}/bo2zm_native_entities.cpp
        ${BO2ZM_SRC}/bo2zm_native_audio.cpp
        ${BO2ZM_SRC}/bo2zm_native_limiter.cpp
        ${BO2ZM_SRC}/bo2zm_native_lzx.cpp
        ${BO2ZM_SRC}/bo2zm_native_skin.cpp)
    list(APPEND BO2CP_SOURCES ${BO2CP_NATIVE_SOURCES}
        src/native/bo2cp_native_stubs.cpp
        ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
        ${REXSDK_DIR}/src/graphics/format/ucode.cpp
        ${REXSDK_DIR}/src/graphics/register_file.cpp)
    # The menu changes (games/bo2/src/ui/menus.lua, the Zombies program's file: it leaves its Zombies-only parts out in the
    # campaign) go into the program as text; bo2zm_ui.cpp runs them inside the game's menu Lua.
    set(BO2CP_UI_LUA "${BO2ZM_SRC}/ui/menus.lua")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BO2CP_UI_LUA}")
    file(READ "${BO2CP_UI_LUA}" BO2ZM_UI_LUA_TEXT)
    configure_file("${BO2ZM_SRC}/ui/bo2zm_ui_lua.h.in" "${CMAKE_CURRENT_BINARY_DIR}/generated_ui/bo2zm_ui_lua.h" @ONLY)
    set_source_files_properties(${BO2CP_NATIVE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/native/bo2cp_game_remap.h")
    # The build identifier every log names (bo2zm_switch.cpp's BuildId).
    string(TIMESTAMP BO2CP_BUILD_TIME "%Y%m%d-%H%M%S")
    set_property(SOURCE ${BO2ZM_SRC}/bo2zm_switch.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"bo2cp-${BO2CP_BUILD_TIME}\"")
    # The limiter's rewrite repeats the game's single-precision operations one by one: GCC must not fuse a product into
    # a later add there (games/bo2/CMakeLists.txt does the same).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_property(SOURCE ${BO2ZM_SRC}/bo2zm_native_limiter.cpp APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
    endif()
endif()

if(WIN32)
    add_executable(bo2cp WIN32 ${BO2CP_SOURCES})
else()
    add_executable(bo2cp ${BO2CP_SOURCES})
endif()

# Windows: the same icon as bo2zm.exe (icon resource 1). It carries the game's logo, so build_windows.sh passes it
# from the game data folder when it is there.
set(BO2CP_EXE_ICON "" CACHE FILEPATH "Windows .ico for bo2cp.exe")
if(WIN32 AND BO2CP_EXE_ICON AND EXISTS "${BO2CP_EXE_ICON}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bo2cp_icon.rc"
        CONTENT "1 ICON \"@BO2CP_EXE_ICON@\"\n" @ONLY)
    target_sources(bo2cp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bo2cp_icon.rc")
endif()

rexglue_setup_target(bo2cp GPU_PLUGINS xenos)

# How the generated code reaches guest memory: the Zombies program's header (core/bo2zm_guest_memory.h), appended to
# its precompiled header, defines REX_GUEST_BARRIER() for the lines tools/guest_barriers.py places; with
# BO2CP_GUEST_MEMORY_PLAIN the guest loads and stores are plain accesses.
option(BO2CP_GUEST_MEMORY_PLAIN "Generated code: plain guest loads and stores, with compiler barriers on guest memory"
       OFF)
if(TARGET bo2cp_recomp)
    target_precompile_headers(bo2cp_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../core/bo2zm_guest_memory.h")
    if(BO2CP_GUEST_MEMORY_PLAIN)
        target_compile_definitions(bo2cp_recomp PRIVATE BO2ZM_GUEST_MEMORY_PLAIN=1)
    endif()
endif()

# rex_app.cpp (compiled into this target) includes imgui.h, but rexruntime links
# imgui privately, so the include path does not reach us when the SDK is a
# subdirectory (games/bo2/CMakeLists.txt does the same).
if(TARGET imgui)
    target_include_directories(bo2cp PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

if(BO2CP_NATIVE)
    target_compile_definitions(bo2cp PRIVATE BO2CP_NATIVE=1)
    target_include_directories(bo2cp PRIVATE "${BO2ZM_SRC}" "${BO2ZM_CORE_SRC}" "${BO2ZM_SRC}/native" "${REXSDK_DIR}/src/ui/shaders"
                               "${CMAKE_CURRENT_BINARY_DIR}/generated_ui")
    find_path(BO2CP_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    target_include_directories(bo2cp PRIVATE "${BO2CP_XXHASH_INCLUDE}")
    if(TARGET renderdoc)
        target_include_directories(bo2cp PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
    if(TARGET Vulkan::Headers)
        target_link_libraries(bo2cp PRIVATE Vulkan::Headers)
    endif()
    # The .webm videos' VP8 decoder (bo2zm_webm.cpp) is the SDK's FFmpeg, as in games/bo2/CMakeLists.txt.
    target_link_libraries(bo2cp PRIVATE $<LINK_ONLY:libavcodec> $<LINK_ONLY:libavutil>)
    set_source_files_properties(${BO2ZM_SRC}/bo2zm_webm.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${REXSDK_DIR}/thirdparty/FFmpeg")
    # Every missing symbol in one link, not lld-link's first 20.
    if(WIN32)
        target_link_options(bo2cp PRIVATE "LINKER:/errorlimit:0")
    endif()
endif()
