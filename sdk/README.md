# sdk: ReXGlue SDK with a Horizon layer

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) release **v0.10.0** (commit `c94f5eb`) by Tom Clay, which
carries Xenia's implementation of the Xbox 360's kernel, system libraries and GPU, plus a layer for Horizon (the
Switch's operating system) and this project's changes. `README.upstream.md` is the release's README. Upstream code here
keeps its BSD 3-Clause license (`LICENSE`); this project's changes are GPL-3.0 under the top-level
[`LICENSE`](../LICENSE). See [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

Against the release (line endings ignored): 96 of its files carry changes and 41 files are new, nearly all of them the
Horizon layer. The release's tests, CI and developer scripts are left out. `thirdparty/` holds only the files this
project changed; `python tools/fetch_thirdparty.py` fetches the rest. To see every change:

```bash
git clone https://github.com/rexglue/rexglue-sdk.git rexglue-sdk && git -C rexglue-sdk checkout c94f5eb
git diff --no-index --ignore-cr-at-eol rexglue-sdk/src sdk/src
```

## The Horizon layer (new files)

| Area | Files | What it does |
|---|---|---|
| Guest memory | `src/core/guest_memory_switch.*`, `memory_switch.cpp`, `mapped_memory_switch.cpp` | The 360's memory map on Horizon: an empty reserved window, chunks committed as code-memory aliases and mapped into every view on first access, "protection" by unmapping. Design notes at the top of `guest_memory_switch.h` |
| Threads | `src/core/threading_switch.*` | Horizon threads with names, priorities and core masks, settable from other threads |
| Exceptions | `exception_handler_switch.cpp`, `seh_switch.cpp`, `src/ui/switch_crash_hooks.c` | One libnx fault handler that emulates guest memory accesses without system calls, or writes a crash log |
| Platform shims | `fiber_switch.cpp`, `dynlib_switch.cpp`, `switch_libc_supplement.c`, `switch_elf_stubs.c`, `switch_compat/` | What the SDK expects from a desktop OS, for libnx |
| App, window, display | `src/ui/windowed_app_*_switch.cpp`, `window_switch.cpp`, `runtime_switch.cpp`, `surface_switch.h` | The application, its window and the Vulkan surface |
| Audio, input | `src/audio/switch/`, `src/input/switch/` | `audout` output; HID controllers as Xbox 360 controllers |
| CPU cores | `src/ui/switch_core3.cpp` | Fourth-core placement rules, guards and the failsafe |
| Profiler | `src/ui/switch_perf.cpp` | Per-thread CPU, system calls, clocks, GPU time and stack sampling into `rex_profile.log` |
| System | `switch_apm.cpp`, `switch_sysclk.cpp`, `switch_saltynx.cpp` | Performance mode, clock readings for the logs, SaltyNX detection |
| Logging | `src/core/copyable_file_sink.h` | A writer thread, so logging threads never wait for the SD card |
| Build | `cmake/rexglue_switch.cmake` | devkitA64 builds, linking NVK, libnx and the libraries |

## Changes to the release's files

| Area | What changed |
|---|---|
| Code generator | Register options, the plain guest memory form, and a fix to `vpkuwus`/`vpkuhus`, which corrupted their result when the destination was also a source |
| Sync objects and threads | Events, semaphores and mutants on Horizon's address arbiter; handle lookups without the global lock; audio thread placement |
| Clock | A lock-free guest clock |
| Memory | Hooks for the Horizon memory layer; a crash log on Windows |
| Files | A read cache, paced image pack reads, reports of the most-read files |
| Content and profiles | DLC listed and mounted as the game expects, XSession support so the solo lobby can start, profile settings, text input |
| Sockets | One receive buffer per thread; post-sleep network errors reported as "no data yet" |
| Audio | The decoded XMA frame cache; no per-frame FFmpeg side data |
| GPU | Hooks for the native renderer; changes to the PC's Vulkan back end |
| Presenter and UI | Vulkan on NVK and Horizon; the output pass's FPS counter and credits line |
| Logging | The writer thread, the quiet player log, rotation |

In `thirdparty/`, only FFmpeg's files changed: its configuration for AArch64 Horizon and a WMV3/VP8-only build, the
codec list, and a cache of the FFT and MDCT tables.

## How it is built

Not on its own: `games/bo2/build_switch.sh` and `games/bo2/build_windows.sh` point `REXSDK_DIR` here and CMake builds the SDK with
the app. `tools/build_host.sh` builds the host tools (`rexglue.exe`, `xexdump`). See
[`../docs/building.md`](../docs/building.md).
