# games/bo2

The game: the code generator's configuration, the port's own code and the scripts that build and run it. The
recompiled game is generated into `generated/` and never committed.

## Scripts and configuration

| File | What it is |
|---|---|
| `build_switch.sh` | Builds `out/switch/bo2zm.nro` (GCC 16, `-O3`, LTO; PGO with `BO2_PGO=use`); see [building](../../docs/building.md#step-7-build-the-switch-nro) |
| `build_windows.sh` | Builds `out/win/bo2zm.exe` (clang; Direct3D 12 and Vulkan) |
| `build_linux.sh` | Builds the Linux ARM64 package; see [building-linux-arm64](../../docs/building-linux-arm64.md) |
| `run_windows.sh`, `play_windows.cmd` | Run the PC build, one copy at a time; a double-click shortcut to play |
| `CMakeLists.txt` | The build: generated code, SDK, port sources, the menu script (at most 80 top-level locals), PGO and test builds |
| `bo2zm_manifest.toml` | The code generator's project |
| `overrides.toml` | What the generator is told by hand, each entry with its evidence (root keys above `[functions]`) |
| `gaps.toml` | Code no function covers, from `tools/codegen_gaps.py` |
| `locals.txt` | Hot leaf functions `tools/localize.py` copies with local registers |
| `switch/bo2zm.toml` | The settings file next to the NRO ([features](../../docs/features.md#bo2zmtoml)) |

Generate code only through `tools/codegen.sh`; the builds check for its fix-up passes.

## Source (`src/`)

| Area | Files |
|---|---|
| Start-up and glue | `main.cpp`, `bo2zm_register.cpp`; `bo2zm_guest.h` (calling game functions); `bo2zm_guest_memory.h`; `bo2zm_autoexec.cpp`, `bo2zm_commands.cpp`, `bo2zm_dvars.cpp` (the game's console, the port's commands, dvars) |
| Switch | `bo2zm_switch.cpp` (display, VSYNC), `bo2zm_switch_mutex.cpp`, `bo2zm_relaunch.cpp`, `kernel/` |
| Native rewrites | `bo2zm_native_cull.cpp`, `bo2zm_native_skin.cpp`, `bo2zm_native_lzx.cpp`, `bo2zm_native_audio.cpp` (also the checked-phase framework), `bo2zm_native_limiter.cpp`, `bo2zm_native_entities.cpp`, `bo2zm_stream_alloc.cpp`, `bo2zm_webm.cpp` (VP8 through FFmpeg) |
| Memory and waits | `bo2zm_crt.cpp` and `bo2zm_page_writes.cpp` (native copies and write stamps), `bo2zm_volume_writes.cpp`, `bo2zm_waits.cpp` and `bo2zm_jobs.cpp` (the job system), `bo2zm_sound_waits.cpp`, `bo2zm_gpu_skin.cpp` |
| Direct3D layer | `bo2zm_d3d_marker.cpp` (markers, draw records), `bo2zm_d3d_native.cpp` (native functions), `bo2zm_d3d_trace.cpp` (timing, tracing, the D3D shadow) |
| Renderer | `native/bo2zm_native_system.cpp`; in `native/render/`: `bo2zm_native_render.cpp` (the ring thread), `bo2zm_native_draws.cpp` (draws and caches), `bo2zm_native_deferred.cpp` (recorder), `bo2zm_native_targets.cpp` (targets, resolves, present), `bo2zm_native_shaders.cpp` and `bo2zm_shader_library.cpp`, `bo2zm_native_texture_pool.cpp`, `bo2zm_native_precompile.cpp`, `bo2zm_native_hooks.cpp`, `bo2zm_native_capture.cpp` |
| Player features | `ui/menus.lua` and `bo2zm_ui.cpp` (the pages), `bo2zm_preset.cpp`, `bo2zm_settings.cpp`, `bo2zm_extras.cpp`, `bo2zm_cheats.cpp`, `bo2zm_dlc.cpp`, `bo2zm_name.cpp`, `bo2zm_pad.cpp`, `bo2zm_credits.cpp` and `ui/credits/`, `bo2zm_debug_options.cpp` |
| Testing | `bo2zm_benchmark.cpp`, `bo2zm_test_pad.cpp`, `bo2zm_test_waits.cpp`, `bo2zm_pc_profiler.cpp`, `bo2zm_debug_watch.cpp`, `bo2zm_heap_track.cpp`, `bo2zm_memcpy_callers.cpp`, `debug_party.cpp` |
