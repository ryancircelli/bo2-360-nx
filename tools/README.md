# tools

Run from the repository root in Git Bash. Python tools that read game files run with `python -I`, and nothing derived
from the game is ever written inside the repository.

## Build

| Tool | What it does | Run |
|---|---|---|
| `fetch_thirdparty.py` | Fetches the SDK's third-party sources from the ReXGlue v0.10.0 release, keeping this project's changes | `python tools/fetch_thirdparty.py` |
| `apply_thirdparty_patches.sh` | Applies `patches/<library>-*.patch` under `$THIRDPARTY_ROOT` (default `sdk/thirdparty`), skipping patches already applied. The SDK configure runs it on its own per-build copy of SDL | `tools/apply_thirdparty_patches.sh` |
| `build_host.sh` | Builds `rexglue.exe` (code generator) and `xexdump` | `tools/build_host.sh` |
| `build_mesa.sh` | Builds NVK for the Switch from mesa-switch plus `patches/mesa-bo2zm.patch` | `tools/build_mesa.sh` (`MESA_DIR`, no spaces) |
| `build_zstd_switch.sh` | A static zstd for the Switch, into the Mesa SDK | `tools/build_zstd_switch.sh` |
| `switch/cmake/`, `mesa/` | The devkitA64 toolchain file; Meson's MSYS2 native file | used by the builds |
| `linux/` | The ARM64 sysroot, toolchain, Dockerfile, package and QEMU smoke test | see [building-linux-arm64](../docs/building-linux-arm64.md) |
| `bootstrap_worktree.sh` | Prepares a git worktree to build beside the main checkout | `tools/bootstrap_worktree.sh <tag> [--own-generated]` |

## Game files and the shader library

| Tool | What it does | Run |
|---|---|---|
| `xexdump/` | Writes the decrypted, patched executable image and a summary | `xexdump.exe <game_dir> <xex_name> <out_dir>` |
| `install_dlc.py` | Unpacks DLC packages into the layout the game reads | `python -I tools/install_dlc.py <packages> --out "$BO2_DATA/content" [--all]` |
| `game_files/prepare_game_files.py` | Lays out your disc, Title Update 18 and DLC as `switch/bo2zm/{game,update,user}`, ready to merge onto an SD card that already has the game; `game_files/make_zip.py` packs it with its helpers and README | `python tools/game_files/prepare_game_files.py "<your disc folder>" [--out <folder>]` |
| `inspect/ff_shaders.py` | Decrypts fastfiles (key from your decrypted image) and dumps their shaders | `python -I tools/inspect/ff_shaders.py <file.ff> ... --dump --out DIR` |
| `xenosrecomp/run.sh` | Translates the shaders to HLSL (XenosRecomp), compiles them with `dxc`, validates them | `tools/xenosrecomp/run.sh <blobs> <new output folder>` |
| `xenosrecomp/bo2zm_pack.cpp`, `bo2zm_lib_dump.cpp` | Packs the shader library and writes its index (`--index`); extracts entries from one | see [building](../docs/building.md#step-5-build-the-shader-library) |
| `stage_sd.sh`, `stage_update.sh` | The SD card folder; only what changed since the last copy | `tools/stage_sd.sh [out_dir]`, `tools/stage_update.sh` |

## Code generation

`codegen.sh` runs the generator and these passes in order.

| Tool | What it does | Run |
|---|---|---|
| `codegen.sh` | Generator, fix-up passes, stats | `tools/codegen.sh` |
| `read_before_write.py` | Registers read before written; `--strict` stops on unknown cases | `python tools/read_before_write.py [--strict]` |
| `setjmp_locals.py` | `setjmp` sites restore register locals | `--fix` or `--verify` |
| `direct_calls.py` | Direct calls to unhooked functions; `--check` catches hooked ones | `[--check \| --undo]` |
| `localize.py` | Local-register copies of the functions in `games/bo2/locals.txt` | `python tools/localize.py` |
| `guest_barriers.py` | Compiler barriers for plain guest memory | `[--check \| --undo]` |
| `codegen_stats.py` | Counts what makes generated code slow | `python tools/codegen_stats.py` |
| `codegen_fix_unresolved.py` | Declares tail-branch targets that are in no function | `[--max-rounds N]` |
| `codegen_gaps.py` | Classifies code no function covers into `games/bo2/gaps.toml` | `[--min 8]` |
| `volatile_locals.py` | Experimental: volatile registers as locals (+0.07 %, not used) | `[--check]` |
| `gen_d3d_trace_list.py` | Lists the Direct3D functions the trace wrappers cover | `python tools/gen_d3d_trace_list.py` |

## Measuring

| Tool | What it does | Run |
|---|---|---|
| `pgo_check.py` | How many trained functions still read a PGO profile's counts | `--build games/bo2/out/switch --pgo <profile> [--native]` |
| `profile_sym.py` | Names the console profiler's addresses | `rex_profile.log <elf> out.txt` |
| `profile_inline.py` | One thread's samples by inlined function or source line | `rex_profile.log <lines elf> [--thread NAME] [--lines]` |
| `memcpy_callers_sym.py` | A copy-callers build's calls by call site | `<game log> <lines elf>` |
| `pc_profile_report.py` | The PC profiler's hottest functions per thread | `<pc_profile_<pid>.txt>` |
| `rdc_cost.py` | Draws and GPU time of RenderDoc captures (inside `qrenderdoc`) | `RDC_GLOB`, `RDC_OUT` |
| `spot_flicker.py` | What blinks in a burst of test screenshots | `<captures folder> [--burst NAME]` |
| `pipelines_list.py` | Reads, merges and subtracts pipeline prewarm lists | `info \| extract \| merge \| minus` |
| `check_menus_lua.py` | Checks `menus.lua` with the Lua 5.1 compiler | `python -I tools/check_menus_lua.py` |
| `pc_desktop_state.ps1` | Counts screens, for `games/bo2/run_windows.sh` | used by the script |
| `runtime_stress/` | The kernel objects' waits under stress on Linux | `tools/runtime_stress/build.sh` (in WSL) |

## Inspecting the game

These read your own files or the generated code and write nothing into the repository.

| Tool | What it does |
|---|---|
| `inspect/xex_header.py`, `xexp_info.py` | XEX headers and XEX/XEXP versions |
| `inspect/stfs_extract.py` | Lists or extracts STFS packages |
| `inspect/ppc_dis.py`, `ppc_xref.py` | PowerPC disassembly; code that builds an address |
| `inspect/gen_index.py`, `genxref.py`, `genfn.sh` | The generated code by function: calls, callers, cross-references, one function |
| `inspect/dvar_list.py` | The game's dvars and their flags |
| `inspect/gsc_list.py` | Compiled game scripts in a zone: exports, imports, strings, calls |
| `inspect/hks_dis.py` | The game's compiled menu Lua as pseudo-code |
| `inspect/grep_strings.py` | A pattern search in a binary |

## Artwork

`make_credits.py` draws the home screen's credits line: `python -I tools/make_credits.py [--font PATH] [--out DIR]`.
