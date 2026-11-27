# bo2zm: Black Ops II Zombies, recompiled for the Mac.
#
# The program has two parts. bo2zm_recomp is the game's code, translated from PowerPC to C++ by the ReXGlue code
# generator (tools/codegen.sh writes it to generated/, from the player's own copy of the game). bo2zm is everything
# written by hand around it: the hooks that replace or extend game functions, the native renderer, the sound and
# streaming work, the menus' Lua, and the platform layer for each system. The SDK's boilerplate for both targets is
# in generated/rexglue.cmake; tools/build_mac.sh configures and builds them.

cmake_minimum_required(VERSION 3.25)
project(bo2zm LANGUAGES CXX)

# The physical games/bo2 directory. The repository root has an app link to it for old scripts, and a configure through that
# link must still find core/ and sdk/ next to the real directory, so every path below starts here.
file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" BO2ZM_ROOT)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${BO2ZM_ROOT}/generated/rexglue.cmake")

# Sources: games/bo2/ is the BO2 build project; core/ holds the game-neutral services and games/bo2/src/ the BO2 code. Both
# directories are on the include path, so "bo2zm_x.h" resolves whichever one it lives in.
set(BO2ZM_SOURCES
    ${BO2ZM_ROOT}/src/main.cpp
    ${BO2ZM_ROOT}/src/bo2zm_register.cpp
    ${BO2ZM_ROOT}/src/kernel/xboxkrnl_usbcam.cpp
    ${BO2ZM_ROOT}/src/bo2zm_relaunch.cpp
    ${BO2ZM_ROOT}/src/debug_party.cpp
    ${BO2ZM_ROOT}/src/bo2zm_autoexec.cpp
    ${BO2ZM_ROOT}/src/bo2zm_dlc.cpp
    ${BO2ZM_ROOT}/src/bo2zm_gsc_mods.cpp
    ${BO2ZM_ROOT}/src/bo2zm_mod_hud_fit.cpp
    ${BO2ZM_ROOT}/src/bo2zm_script_manager.cpp
    ${BO2ZM_ROOT}/src/bo2zm_bots.cpp
    ${BO2ZM_ROOT}/src/bo2zm_launch_options.cpp
    ${BO2ZM_ROOT}/src/bo2zm_local_order.cpp
    ${BO2ZM_ROOT}/src/bo2zm_ui.cpp
    # The sound system's wait for a zone's sound banks, with a shorter sleep.
    ${BO2ZM_ROOT}/src/bo2zm_sound_waits.cpp
    ${BO2ZM_ROOT}/src/bo2zm_commands.cpp
    ${BO2ZM_ROOT}/src/bo2zm_cheats.cpp
    ${BO2ZM_ROOT}/src/bo2zm_cheat_sync.cpp
    ${BO2ZM_ROOT}/src/bo2zm_player_projection.cpp
    ${BO2ZM_ROOT}/src/bo2zm_dvars.cpp
    ${BO2ZM_ROOT}/src/bo2zm_extras.cpp
    ${BO2ZM_ROOT}/src/bo2zm_name.cpp
    # Internet play from the home menu: the menu side of the SDK's online_link (UPnP, STUN, join codes).
    ${BO2ZM_ROOT}/src/bo2zm_online.cpp
    ${BO2ZM_ROOT}/../../core/bo2zm_pc_profiler.cpp
    ${BO2ZM_ROOT}/src/bo2zm_controllers.cpp
    ${BO2ZM_ROOT}/src/bo2zm_pad.cpp
    ${BO2ZM_ROOT}/../../core/bo2zm_settings.cpp
    ${BO2ZM_ROOT}/src/bo2zm_preset.cpp
    ${BO2ZM_ROOT}/src/bo2zm_benchmark.cpp
    ${BO2ZM_ROOT}/src/bo2zm_perf_log.cpp
    ${BO2ZM_ROOT}/../../core/bo2zm_heap_track.cpp
    ${BO2ZM_ROOT}/../../core/bo2zm_memcpy_callers.cpp
    ${BO2ZM_ROOT}/src/bo2zm_debug_options.cpp
    ${BO2ZM_ROOT}/src/bo2zm_test_waits.cpp
    ${BO2ZM_ROOT}/src/bo2zm_sound_mix.cpp
    ${BO2ZM_ROOT}/src/bo2zm_native_audio.cpp
    ${BO2ZM_ROOT}/src/bo2zm_native_limiter.cpp
    ${BO2ZM_ROOT}/src/bo2zm_webm.cpp
    ${BO2ZM_ROOT}/src/bo2zm_native_lzx.cpp
    ${BO2ZM_ROOT}/../../core/bo2zm_page_writes.cpp
    ${BO2ZM_ROOT}/src/bo2zm_native_skin.cpp
    ${BO2ZM_ROOT}/src/bo2zm_gpu_skin.cpp
    ${BO2ZM_ROOT}/src/bo2zm_native_cull.cpp
    # The entity loops: a read-only scan pass before sub_822FD360, the plane test sub_822FE3B8 natively.
    ${BO2ZM_ROOT}/src/bo2zm_native_entities.cpp
    ${BO2ZM_ROOT}/src/bo2zm_stream_alloc.cpp
    # BO2's memcpy/memmove/memset as native code ([rexcrt] in overrides.toml)
    ${BO2ZM_ROOT}/src/bo2zm_crt.cpp
    ${BO2ZM_ROOT}/src/bo2zm_test_pad.cpp
      ${BO2ZM_ROOT}/src/bo2zm_party_probe.cpp
    ${BO2ZM_ROOT}/src/bo2zm_d3d_trace.cpp
    ${BO2ZM_ROOT}/src/bo2zm_d3d_marker.cpp
    # The game's D3D functions in native code, each checked against the game's own result first.
    ${BO2ZM_ROOT}/src/bo2zm_d3d_native.cpp
    ${BO2ZM_ROOT}/src/native/bo2zm_native_system.cpp
    ${BO2ZM_ROOT}/src/bo2zm_switch.cpp
    # The home screen's credits line, drawn over the menu.
    ${BO2ZM_ROOT}/src/bo2zm_credits.cpp
    # PC diagnostics: who writes a range of guest memory (bo2zm_debug_watch_volume).
    ${BO2ZM_ROOT}/src/bo2zm_debug_watch.cpp
    # The lighting volume's writer hooked: the renderer takes its marks instead of hashing the volume.
    ${BO2ZM_ROOT}/src/bo2zm_volume_writes.cpp
    # The native renderer (games/bo2/src/native/render, derived from GPL-3.0 code; see
    # THIRD_PARTY_NOTICES.md): render targets and copies, draws, the
    # shader library (shaders, shader_library), shader identity from the game
    # thread (hooks) and the system.
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_render.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_targets.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_draws.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_deferred.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_shaders.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_shader_library.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_shader_index.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_progress.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_hooks.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_texture_pool.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_precompile.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_capture.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_video.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_native_pso.cpp
    ${BO2ZM_ROOT}/src/native/render/bo2zm_graphics_settings.cpp
    ${BO2ZM_ROOT}/src/native/bo2zm_render_stubs.cpp
    # The SDK's Xenos shader interpreter, for the vertex shaders that write memory (memexport): BO2 copies
    # streamed texture data with them. These files belong to the Xenos GPU plugin, which the app does not link
    # (it is loaded at runtime on PC), so the native renderer compiles its own copy.
    ${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp
    ${REXSDK_DIR}/src/graphics/format/ucode.cpp
    ${REXSDK_DIR}/src/graphics/register_file.cpp
)

# BO2ZM_DEVLINK: the PC link for development consoles, which take new builds from a PC, send their logs back and run a
# watchdog during benchmarks; their home screen gets CHECK FOR UPDATES against that PC. Off, the program has no code
# for it (games/bo2/src/bo2zm_lifecycle.cpp stands in) and the menus leave the button out. A tree without its sources is always
# built without it.
if(EXISTS "${BO2ZM_ROOT}/src/bo2zm_devlink.cpp")
    option(BO2ZM_DEVLINK "Development consoles: the link to a PC for builds and logs, and CHECK FOR UPDATES" ON)
else()
    set(BO2ZM_DEVLINK OFF CACHE BOOL "Development consoles: the link to a PC (its sources are not in this tree)" FORCE)
endif()
message(STATUS "bo2zm: BO2ZM_DEVLINK ${BO2ZM_DEVLINK}")
if(BO2ZM_DEVLINK)
    list(APPEND BO2ZM_SOURCES ${BO2ZM_ROOT}/src/bo2zm_devlink.cpp ${BO2ZM_ROOT}/src/bo2zm_updates.cpp)
else()
    list(APPEND BO2ZM_SOURCES ${BO2ZM_ROOT}/src/bo2zm_lifecycle.cpp)
endif()

if(WIN32)
    add_executable(bo2zm WIN32 ${BO2ZM_SOURCES})
else()
    add_executable(bo2zm ${BO2ZM_SOURCES})
endif()

# Linux (games/bo2/build_linux.sh): the page-size check and the relaunch.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(bo2zm PRIVATE ${BO2ZM_ROOT}/../../core/bo2zm_linux.cpp)
endif()
# macOS (tools/build_mac.sh): the relaunch.
if(APPLE)
    target_sources(bo2zm PRIVATE ${BO2ZM_ROOT}/../../core/bo2zm_mac.cpp)
endif()

if(BO2ZM_DEVLINK)
    # The PC build checks for updates against a PC of its own (CHECK FOR UPDATES, bo2zm_updates.cpp) but never
    # downloads: the network code without zlib, which this build does not have.
    target_sources(bo2zm PRIVATE ${BO2ZM_ROOT}/src/bo2zm_devlink_net.cpp)
    set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_devlink_net.cpp PROPERTIES
                                COMPILE_DEFINITIONS "BO2ZM_DEVLINK_NO_ZLIB=1;_CRT_SECURE_NO_WARNINGS")
endif()
if(WIN32)
    target_link_libraries(bo2zm PRIVATE ws2_32)
endif()

# xenos: ReXGlue's emulated Xbox 360 GPU, which the PC build can use instead of the native renderer.
rexglue_setup_target(bo2zm GPU_PLUGINS xenos)

# The game-neutral services (core/) and the BO2 sources (games/bo2/src/): a quoted "bo2zm_x.h" include finds its file in
# either directory, as it did when both were app/src.
target_include_directories(bo2zm PRIVATE "${BO2ZM_ROOT}/../../core" "${BO2ZM_ROOT}/src")

# Hot leaf functions copied with their ctx registers as locals (tools/localize.py, from games/bo2/locals.txt; run by
# tools/codegen.sh). Same target as the generated code, so the same flags and precompiled header.
set(BO2ZM_LOCALS "${BO2ZM_ROOT}/generated/default_mp/bo2zm_locals.cpp")
if(EXISTS "${BO2ZM_LOCALS}" AND TARGET bo2zm_recomp)
  target_sources(bo2zm_recomp PRIVATE "${BO2ZM_LOCALS}")
  target_compile_definitions(bo2zm PRIVATE BO2ZM_HAS_LOCALS=1)
endif()

# How the generated code reaches guest memory (core/bo2zm_guest_memory.h): appended to its precompiled header, so
# REX_GUEST_BARRIER() (the lines tools/guest_barriers.py puts after labels and ordering instructions) is defined; with
# BO2ZM_GUEST_MEMORY_PLAIN the guest loads and stores are plain accesses instead of volatile ones.
option(BO2ZM_GUEST_MEMORY_PLAIN "Generated code: plain guest loads and stores, with compiler barriers on guest memory"
       OFF)
if(TARGET bo2zm_recomp)
  target_precompile_headers(bo2zm_recomp PRIVATE "${BO2ZM_ROOT}/../../core/bo2zm_guest_memory.h")
  rexglue_patchable_all(bo2zm_recomp)   # test builds: -DREXGLUE_PATCHABLE_ALL=ON (default OFF)
  if(BO2ZM_GUEST_MEMORY_PLAIN)
    target_compile_definitions(bo2zm_recomp PRIVATE BO2ZM_GUEST_MEMORY_PLAIN=1)
  endif()
  # macOS: the function registration table (1.6 MB, run once at startup) takes Apple clang 30 minutes at -O3 on a
  # loaded machine. At -O0, without the PCH and without GlobalISel it takes seconds and runs as fast. The idea comes
  # from the Fable II Recomp for macOS project.
  # The path must be spelled as generated/rexglue.cmake lists the source (CMAKE_CURRENT_SOURCE_DIR, which is app/ when
  # configured through the link), or the properties land on a different source object and nothing changes.
  if(APPLE)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/generated/default_mp/bo2zm_register.cpp"
                                TARGET_DIRECTORY bo2zm_recomp
                                PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
  endif()
endif()

# rex_app.cpp (compiled into this target) includes imgui.h, but rexruntime links
# imgui privately, so the include path does not reach us when the SDK is a
# subdirectory.
if(TARGET imgui)
    target_include_directories(bo2zm PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# The same goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes
# (games/bo2/src/native/ uses the SDK's Vulkan device).
if(TARGET renderdoc)
    target_include_directories(bo2zm PRIVATE
        $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# And the Vulkan headers, which the SDK's ui/graphics libraries use privately.
if(TARGET Vulkan::Headers)
    target_link_libraries(bo2zm PRIVATE Vulkan::Headers)
endif()

# The loading videos' VP8 decoder (games/bo2/src/bo2zm_webm.cpp) is the SDK's FFmpeg. Its headers for that file only: the FFmpeg
# folder has its own config.h.
target_link_libraries(bo2zm PRIVATE $<LINK_ONLY:libavcodec> $<LINK_ONLY:libavutil>)
set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_webm.cpp PROPERTIES INCLUDE_DIRECTORIES "${REXSDK_DIR}/thirdparty/FFmpeg")

# The native renderer (games/bo2/src/native/render) draws the game image with the SDK
# presenter's precompiled SPIR-V, and fingerprints with the SDK's xxHash.
target_include_directories(bo2zm PRIVATE "${REXSDK_DIR}/src/ui/shaders")
find_path(BO2ZM_XXHASH_INCLUDE xxhash.h HINTS "${REXSDK_DIR}/thirdparty/xxHash" NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(bo2zm PRIVATE "${BO2ZM_XXHASH_INCLUDE}")

# This port's menu changes (games/bo2/src/ui/menus.lua) go into the program as text: bo2zm_ui.cpp runs them inside the game's
# menu Lua. Editing menus.lua re-runs the configure step, which rewrites the header only when the text changed.
set(BO2ZM_UI_LUA "${BO2ZM_ROOT}/src/ui/menus.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BO2ZM_UI_LUA}")
file(READ "${BO2ZM_UI_LUA}" BO2ZM_UI_LUA_TEXT)
# Without BO2ZM_DEVLINK the home menu has no CHECK FOR UPDATES (menus.lua, UpdatesButton).
if(NOT BO2ZM_DEVLINK)
    string(FIND "${BO2ZM_UI_LUA_TEXT}" "local UpdatesButton = true\n" _bo2zm_updates_at)
    if(_bo2zm_updates_at LESS 0)
        message(FATAL_ERROR "games/bo2/src/ui/menus.lua: the line \"local UpdatesButton = true\" is missing")
    endif()
    string(REPLACE "local UpdatesButton = true\n" "local UpdatesButton = false\n" BO2ZM_UI_LUA_TEXT
           "${BO2ZM_UI_LUA_TEXT}")
endif()
# The game's Lua compiler writes over the menu heap when one function has more than about 95 active local variables,
# so the top level's "local" lines (at the left margin) must stay at 80 or fewer (tools/check_menus_lua.py counts the
# names too).
string(REGEX MATCHALL "\nlocal " BO2ZM_UI_LUA_TOP_LOCALS "${BO2ZM_UI_LUA_TEXT}")
list(LENGTH BO2ZM_UI_LUA_TOP_LOCALS BO2ZM_UI_LUA_TOP_LOCAL_COUNT)
if(BO2ZM_UI_LUA_TOP_LOCAL_COUNT GREATER 80)
    message(FATAL_ERROR "games/bo2/src/ui/menus.lua has ${BO2ZM_UI_LUA_TOP_LOCAL_COUNT} top-level local lines (at most 80): put "
                        "helpers that only one section uses in a do ... end block (see the top of menus.lua)")
endif()
configure_file("${BO2ZM_ROOT}/src/ui/bo2zm_ui_lua.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated_ui/bo2zm_ui_lua.h" @ONLY)
# The home screen's credits line (games/bo2/src/ui/credits, drawn by tools/make_credits.py) goes into the program as the bytes of
# its two PNG images; bo2zm_credits.cpp decodes them.
foreach(_bo2zm_lines 720 1080)
    set(_bo2zm_png "${BO2ZM_ROOT}/src/ui/credits/credits_${_bo2zm_lines}.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bo2zm_png}")
    file(READ "${_bo2zm_png}" _bo2zm_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," BO2ZM_CREDITS_${_bo2zm_lines} "${_bo2zm_hex}")
endforeach()
configure_file("${BO2ZM_ROOT}/src/ui/credits/bo2zm_credits_png.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated_ui/bo2zm_credits_png.h" @ONLY)
target_include_directories(bo2zm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_ui")

# The title manifest (games/bo2/game.toml), compiled in as rex::kGameManifest.
rexglue_game_manifest(bo2zm "${BO2ZM_ROOT}/game.toml")

# The sound limiter's rewrite repeats the game's single-precision operations one by one: GCC (whose default is
# -ffp-contract=fast) must not fuse a product into a later add there (games/bo2/src/bo2zm_native_limiter.cpp).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_native_limiter.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_player_projection.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

# App icons. The artwork carries the game's logo, so it is kept outside the repository with the game data;
# build_windows.sh passes it when it is there. Without one the exe has none.
set(BO2ZM_EXE_ICON "" CACHE FILEPATH "Windows .ico for bo2zm.exe")


# Windows: icon resource 1, which Explorer and SDL's window class both use.
if(WIN32 AND BO2ZM_EXE_ICON AND EXISTS "${BO2ZM_EXE_ICON}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc"
        CONTENT "1 ICON \"@BO2ZM_EXE_ICON@\"\n" @ONLY)
    target_sources(bo2zm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc")
endif()

# Build ID logged at startup (games/bo2/src/bo2zm_switch.cpp) and in the perf log's header
# (games/bo2/src/bo2zm_perf_log.cpp), so every log names the build that wrote it. the build scripts pass the time.
set(BO2ZM_BUILD_ID "dev" CACHE STRING "Build identifier shown in the log")
set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_switch.cpp ${BO2ZM_ROOT}/src/bo2zm_perf_log.cpp PROPERTIES
    COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"${BO2ZM_BUILD_ID}\"")
