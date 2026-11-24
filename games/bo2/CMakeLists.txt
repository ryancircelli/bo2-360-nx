# bo2zm: Black Ops II Zombies, recompiled for the Nintendo Switch and for Windows.
#
# The program has two parts. bo2zm_recomp is the game's code, translated from PowerPC to C++ by the ReXGlue code
# generator (tools/codegen.sh writes it to generated/, from the player's own copy of the game). bo2zm is everything
# written by hand around it: the hooks that replace or extend game functions, the native renderer, the sound and
# streaming work, the menus' Lua, and the platform layer for each system. The SDK's boilerplate for both targets is
# in generated/rexglue.cmake; games/bo2/build_switch.sh and games/bo2/build_windows.sh configure and build them.

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
    ${BO2ZM_ROOT}/src/bo2zm_waits.cpp
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
    ${BO2ZM_ROOT}/src/bo2zm_jobs.cpp
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

if(BO2ZM_DEVLINK AND REXGLUE_PLATFORM_SWITCH)
    target_sources(bo2zm PRIVATE ${BO2ZM_ROOT}/src/bo2zm_devlink_net.cpp ${BO2ZM_ROOT}/src/bo2zm_devlink_screen.cpp)
elseif(BO2ZM_DEVLINK)
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
# either directory, as it did when both were app/src. Not the repository root: "core/threading_switch.h" is the SDK's.
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
  if(APPLE)
    set_source_files_properties("${BO2ZM_ROOT}/generated/default_mp/bo2zm_register.cpp"
                                TARGET_DIRECTORY bo2zm_recomp
                                PROPERTIES COMPILE_OPTIONS "-O0;-fno-global-isel" SKIP_PRECOMPILE_HEADERS ON)
  endif()
endif()

# Profile-guided optimization of the generated code (games/bo2/build_switch.sh, BO2_PGO), and of the app's hot native code
# (the list below). Not the SDK: a training build that instruments everything, including the SDK's platform layer with
# its wrapped threadCreate, mutexes and fault handler, hangs at startup on the console, probably from the same
# thread-local problem described below. The profile file names are the object paths relative to the build folder, in
# Windows form (GCC compares the prefix with its working directory, which it gets with backslashes), so the training
# build and the final one look for the same names.
set(BO2ZM_PGO "" CACHE STRING "Switch: profile-guided optimization of the generated code: generate, use or empty")
set(BO2ZM_PGO_DIR "" CACHE PATH "Switch, BO2ZM_PGO=use: the folder with the training run's profile")
if(REXGLUE_PLATFORM_SWITCH AND TARGET bo2zm_recomp AND BO2ZM_PGO)
  file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _bo2zm_pgo_prefix)
  # The app's own hot native code takes the same profile: the renderer (its ring and recorder threads use about 16 ms
  # of CPU per frame on the console), the native game routines, the sound mixer's native stages, skinning, culling and
  # the page-write stamps. Not the platform layer (bo2zm_switch*, the PC link, the benchmark that writes the
  # profile), the job system's waits, or anything that runs before the C runtime is up.
  set(_bo2zm_pgo_native
      ${BO2ZM_ROOT}/src/native/render/bo2zm_native_render.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_targets.cpp
      ${BO2ZM_ROOT}/src/native/render/bo2zm_native_draws.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_deferred.cpp
      ${BO2ZM_ROOT}/src/native/render/bo2zm_native_shaders.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_shader_library.cpp
      ${BO2ZM_ROOT}/src/native/render/bo2zm_native_hooks.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_texture_pool.cpp
      ${BO2ZM_ROOT}/src/native/bo2zm_native_system.cpp ${BO2ZM_ROOT}/src/bo2zm_native_audio.cpp ${BO2ZM_ROOT}/src/bo2zm_native_limiter.cpp
      ${BO2ZM_ROOT}/src/bo2zm_native_skin.cpp ${BO2ZM_ROOT}/src/bo2zm_gpu_skin.cpp ${BO2ZM_ROOT}/src/bo2zm_native_cull.cpp ${BO2ZM_ROOT}/src/bo2zm_native_entities.cpp
      ${BO2ZM_ROOT}/src/bo2zm_native_lzx.cpp ${BO2ZM_ROOT}/../../core/bo2zm_page_writes.cpp ${BO2ZM_ROOT}/src/bo2zm_volume_writes.cpp ${BO2ZM_ROOT}/src/bo2zm_d3d_marker.cpp
      ${BO2ZM_ROOT}/src/bo2zm_d3d_native.cpp ${BO2ZM_ROOT}/src/bo2zm_crt.cpp)
  if(BO2ZM_PGO STREQUAL "generate")
    # -fno-profile-values: devkitA64's prebuilt libgcov reaches its thread-local variables (the indirect-call
    # profiler's) through tpidr_el0, which is 0 on Horizon's threads (libnx keeps its thread data elsewhere), so a
    # training build with value profiling faults at address 0xd68 in __gcov_indirect_call_profiler_v4 on the first
    # guest thread. Without value profiling the counters are plain globals; only indirect-call promotion is lost.
    target_compile_options(bo2zm_recomp PRIVATE -fprofile-generate=/switch/bo2zm/pgo -fno-profile-values
                           -fprofile-update=single "-fprofile-prefix-path=${_bo2zm_pgo_prefix}")
    set_property(SOURCE ${_bo2zm_pgo_native} APPEND PROPERTY COMPILE_OPTIONS -fprofile-generate=/switch/bo2zm/pgo
                 -fno-profile-values -fprofile-update=single "-fprofile-prefix-path=${_bo2zm_pgo_prefix}")
    target_compile_definitions(bo2zm PRIVATE BO2ZM_PGO_GENERATE=1)
    target_link_libraries(bo2zm PRIVATE gcov)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=fopen")
  elseif(BO2ZM_PGO STREQUAL "use")
    target_compile_options(bo2zm_recomp PRIVATE "-fprofile-use=${BO2ZM_PGO_DIR}"
                           "-fprofile-prefix-path=${_bo2zm_pgo_prefix}" -fprofile-partial-training -fprofile-correction
                           -Wno-missing-profile -Wno-coverage-mismatch)
    # A profile trained before these files were in it has none for them: they build as without one.
    set_property(SOURCE ${_bo2zm_pgo_native} APPEND PROPERTY COMPILE_OPTIONS "-fprofile-use=${BO2ZM_PGO_DIR}"
                 "-fprofile-prefix-path=${_bo2zm_pgo_prefix}" -fprofile-partial-training -fprofile-correction
                 -Wno-missing-profile -Wno-coverage-mismatch)
  else()
    message(FATAL_ERROR "BO2ZM_PGO must be generate, use or empty")
  endif()
endif()

# Switch profile builds (BO2_DEBUG_LINES=1 in games/bo2/build_switch.sh): line tables for the native renderer's and audio
# sources and at the LTO link, so tools/profile_sym.py can name the file and line of each sample; -g does not change the
# code GCC generates.
option(BO2ZM_DEBUG_LINES "Switch profile builds: -g1 line tables for the native renderer" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_DEBUG_LINES)
  set_property(SOURCE ${BO2ZM_ROOT}/src/native/render/bo2zm_native_render.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_draws.cpp
               ${BO2ZM_ROOT}/src/native/render/bo2zm_native_targets.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_deferred.cpp
               ${BO2ZM_ROOT}/src/native/render/bo2zm_native_shaders.cpp ${BO2ZM_ROOT}/src/native/render/bo2zm_native_texture_pool.cpp
               ${BO2ZM_ROOT}/src/native/render/bo2zm_native_hooks.cpp ${BO2ZM_ROOT}/src/bo2zm_native_audio.cpp ${BO2ZM_ROOT}/src/bo2zm_native_limiter.cpp
               APPEND PROPERTY COMPILE_OPTIONS -g1)
  if(TARGET rexaudio)  # the SDK's audio (the XMA decoder)
    target_compile_options(rexaudio PRIVATE -g1)
  endif()
  target_link_options(bo2zm PRIVATE -g1)
endif()

# rex_app.cpp (compiled into this target) includes imgui.h, but rexruntime links
# imgui privately, so the include path does not reach us when the SDK is a
# subdirectory.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(bo2zm PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
# The same goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes
# outside Switch (games/bo2/src/native/ uses the SDK's Vulkan device).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
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
# On Switch it also uses the SDK's thread priority/core helpers (src/core/threading_switch.h).
if(REXGLUE_PLATFORM_SWITCH)
    target_include_directories(bo2zm PRIVATE "${REXSDK_DIR}/src")
endif()

# Nintendo Switch: bo2zm.nro next to the ELF. On the SD card:
#   sdmc:/switch/bo2zm/bo2zm.nro + game/ + update/ (games/bo2/src/bo2zm_register.cpp).
# Switch: contended host mutexes and the C heap's lock spin briefly before sleeping (games/bo2/src/bo2zm_switch_mutex.cpp).
if(REXGLUE_PLATFORM_SWITCH)
    target_sources(bo2zm PRIVATE ${BO2ZM_ROOT}/src/bo2zm_switch_mutex.cpp)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=pthread_mutex_lock" "-Wl,--wrap=__malloc_lock,--wrap=__malloc_unlock")
    # The default display's id as libnx opens it, for VSYNC's refresh signal (games/bo2/src/bo2zm_switch.cpp, RefreshEvent).
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=viOpenDisplay")
endif()

# The sound limiter's rewrite repeats the game's single-precision operations one by one: GCC (whose default is
# -ffp-contract=fast) must not fuse a product into a later add there (games/bo2/src/bo2zm_native_limiter.cpp).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_native_limiter.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_player_projection.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()

# Switch test build: the native renderer's ring thread's memcpy, memmove, memset and memcmp calls by call site in the
# log every 10 s (core/bo2zm_memcpy_callers.cpp). On with BO2_MEMCPY_CALLERS=1 bash games/bo2/build_switch.sh; never in a
# player build.
option(BO2ZM_MEMCPY_CALLERS "Switch test build: log the ring thread's memcpy calls by call site" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_MEMCPY_CALLERS)
    set_source_files_properties(${BO2ZM_ROOT}/../../core/bo2zm_memcpy_callers.cpp PROPERTIES COMPILE_DEFINITIONS BO2ZM_MEMCPY_CALLERS=1)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=memcpy,--wrap=memmove,--wrap=memset,--wrap=memcmp")
endif()

# Switch test build: the C heap's live bytes by caller in the log every minute (core/bo2zm_heap_track.cpp). On with
# BO2_HEAP_TRACK=1 bash games/bo2/build_switch.sh; never in a player build.
option(BO2ZM_HEAP_TRACK "Switch test build: log the C heap's live bytes by caller every minute" OFF)
if(REXGLUE_PLATFORM_SWITCH AND BO2ZM_HEAP_TRACK)
    set_source_files_properties(${BO2ZM_ROOT}/../../core/bo2zm_heap_track.cpp PROPERTIES COMPILE_DEFINITIONS BO2ZM_HEAP_TRACK=1
                                COMPILE_OPTIONS -fno-omit-frame-pointer)
    target_link_options(bo2zm PRIVATE "-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign"
                        "-Wl,--wrap=aligned_alloc,--wrap=posix_memalign")
endif()

# App icons. The artwork carries the game's logo, so it is kept outside the repository with the game data;
# build_switch.sh and build_windows.sh pass it when it is there. Without one the NRO gets libnx's default icon and the
# exe none.
set(BO2ZM_NRO_ICON "" CACHE FILEPATH "256x256 JPEG icon for bo2zm.nro (hbmenu)")
set(BO2ZM_EXE_ICON "" CACHE FILEPATH "Windows .ico for bo2zm.exe")

if(REXGLUE_PLATFORM_SWITCH)
    set(_bo2zm_icon_arg)
    if(BO2ZM_NRO_ICON AND EXISTS "${BO2ZM_NRO_ICON}")
        set(_bo2zm_icon_arg ICON "${BO2ZM_NRO_ICON}")
    endif()
    rexglue_switch_add_nro(bo2zm
        NAME "Black Ops II 360 NX"
        AUTHOR "AlvaroEPena"
        VERSION "1.1.0"
        ${_bo2zm_icon_arg})
endif()

# Windows: icon resource 1, which Explorer and SDL's window class both use.
if(WIN32 AND BO2ZM_EXE_ICON AND EXISTS "${BO2ZM_EXE_ICON}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc"
        CONTENT "1 ICON \"@BO2ZM_EXE_ICON@\"\n" @ONLY)
    target_sources(bo2zm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bo2zm_icon.rc")
endif()

# Build ID logged at startup (games/bo2/src/bo2zm_switch.cpp) and in the perf log's header
# (games/bo2/src/bo2zm_perf_log.cpp), so every log names the build that wrote it. build_switch.sh / build_windows.sh pass the time.
set(BO2ZM_BUILD_ID "dev" CACHE STRING "Build identifier shown in the log")
set_source_files_properties(${BO2ZM_ROOT}/src/bo2zm_switch.cpp ${BO2ZM_ROOT}/src/bo2zm_perf_log.cpp PROPERTIES
    COMPILE_DEFINITIONS "BO2ZM_BUILD_ID=\"${BO2ZM_BUILD_ID}\"")
