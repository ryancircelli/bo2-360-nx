# Building from source

This page walks through a build from start to finish: your own game files in, a Windows executable
out (the Mac build: `docs/mac/`). Nothing in this repository contains the game. Every step that reads game data runs on your machine, writes into
folders that git ignores, and refuses to write inside the repository where that could happen by mistake.

The build host is **Windows with Git Bash**. Every script is a Bash script and is run from Git Bash. Linux ARM64 has
[its own page](building-linux-arm64.md).

To just play, use a release kit (the README's [Installing](../README.md#installing) section): it runs these same steps
with fewer tools. This page builds from the repository, step by step. Steps 1-8 build Zombies and Multiplayer
(`default_mp.xex`); the Campaign is a separate program, [below](#campaign).

## What you need

**Your own copy of the game:**

- Call of Duty: Black Ops II for the Xbox 360, dumped from your disc: the disc's files, including `default_mp.xex`.
- Title Update 18, the final update (the package `tu00000002_00000000` in the title's `000B0000` folder). The port
  targets TU18's `default_mp.xex` only.
- Optional: your DLC packs as Xbox 360 content packages (the Zombies maps, and the Multiplayer map packs).

**Tools:**

| Tool | Version used | Used for |
|---|---|---|
| Git for Windows (Git Bash) | any recent | running every script |
| Python 3 | 3.11 or later | code generation fix-ups, game file tools |
| Kitware CMake | 4.x, in `C:\Program Files\CMake\bin` | every C++ build (devkitPro's own CMake is older and must not be the one found) |
| Ninja | 1.13, installed with winget | every C++ build |
| LLVM / clang | 20 or later, in `C:\Program Files\LLVM\bin` | the host tools, the Windows build, the shader translator |
| Vulkan SDK | 1.4.x, in `C:\VulkanSDK\<version>` | `dxc` and `spirv-val` for the shader library |

The build scripts find these tools through `tools/toolchain.sh`, in the default folders above (the newest Vulkan SDK
in `C:\VulkanSDK`, MSYS2 in `C:\msys64`). For other places set `BO2_CMAKE_DIR`, `BO2_NINJA`, `BO2_LLVM_DIR`,
`VULKAN_SDK` or `BO2_MSYS2_DIR`.

**Disk and memory:** the generated C++ is about 145 MB in 140 files, and a link-time optimized build needs
plenty of RAM. Builds limit themselves to 8 parallel jobs (`JOBS=` changes it); 32 GB of RAM is comfortable.

## Folders outside the repository

The scripts and tools read your game data from one folder, called `BO2_DATA` in this guide. They find it the same
way everywhere (`tools/data_dir.sh`): the `BO2_DATA` environment variable when it is set, otherwise the path written on
the single line of `bo2_data.txt` at the repository's root, otherwise the `data/` folder at the root. Both
`bo2_data.txt` and `data/` are ignored by git, so the simplest setup is one line, written once:

```bash
echo "D:/bo2-data" > bo2_data.txt
```

Inside it, the build expects this layout (what goes in each folder is described in step 1):

```
$BO2_DATA/
  game/        the disc's files, plus default.xexp and default_mp.xexp from the title update
  update/      the title update package's contents
  content/     the DLC packs, as tools/install_dlc.py lays them out
  shaders/     the shader library you build in step 5
  sd/          the SD card layout, from tools/stage_sd.sh
```

The repository also uses three git-ignored folders of its own: `work/` (the loaded and decrypted executable),
`games/bo2/generated/` (the recompiled game) and `games/bo2/out/` (build outputs).

## Step 1: prepare your game files

1. Copy the disc's files into `$BO2_DATA/game/`.
2. Extract the title update package with the STFS reader:

   ```bash
   python -I tools/inspect/stfs_extract.py "<path to tu00000002_00000000>" --out "$BO2_DATA/update"
   ```

   Then copy `default.xexp` and `default_mp.xexp` from it next to the executables in `$BO2_DATA/game/`: the game
   patches the executable with the file beside it. The whole package stays in `update/` for its newer fastfiles.
3. Optional, the DLC: put your content packages in one folder and run

   ```bash
   python -I tools/install_dlc.py "<folder with the packages>" --out "$BO2_DATA/content"
   ```

   It unpacks each package into the layout the runtime's content manager reads, keeping only what Zombies in English
   needs. `--multiplayer` keeps the English content of both modes; `--all` keeps every language.

## Step 2: build the host tools

```bash
python tools/fetch_thirdparty.py
tools/apply_thirdparty_patches.sh
tools/build_host.sh
```

`fetch_thirdparty.py` clones the ReXGlue SDK v0.10.0 release with its submodules and copies the third-party sources
into `sdk/thirdparty/`, keeping the few files this project changed. `apply_thirdparty_patches.sh` applies the patches
in `patches/` idempotently. `build_host.sh` then builds two Windows tools:
`rexglue.exe`, the code generator (`sdk/out/win-amd64/`), and `xexdump.exe` (`out/host/switch_bo2/`). Both need
`sdk/out/win-amd64` on `PATH` for their DLLs.

## Step 3: load and decrypt the executable

The code generator reads `work/load_tu18/`, which holds the disc's `default_mp.xex` with TU18's `default_mp.xexp`
beside it:

```bash
mkdir -p work/load_tu18
cp "$BO2_DATA/game/default_mp.xex" "$BO2_DATA/game/default_mp.xexp" work/load_tu18/
export PATH="$PWD/sdk/out/win-amd64:$PATH"
out/host/switch_bo2/xexdump.exe work/load_tu18 default_mp.xex work/dump_tu18
```

`xexdump` loads the executable through the SDK's loader, which decrypts and decompresses the XEX2 image and applies
the title update's delta patch, and writes the image as it sits in guest memory
(`work/dump_tu18/default_mp.image.bin`, 47,054,848 bytes, based at 0x82000000) with a JSON summary. The code
generator does the same load itself; the dumped image is for the shader extraction in step 5 and the inspection tools.

The shader extractor looks for the image under that name. For TU18 its SHA-256 is
`ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e`.

## Step 4: generate the C++

```bash
tools/codegen.sh
```

This runs the code generator on `games/bo2/bo2zm_manifest.toml` and then the fix-up passes the build depends on, in order:

1. `rexglue codegen`: about 30,100 functions into `games/bo2/generated/default_mp/` (about 145 MB of C++, log in
   `games/bo2/codegen.log`).
2. `tools/read_before_write.py --strict`: stops if a function reads a register before writing it and is not a
   known case.
3. `tools/setjmp_locals.py --fix`: register locals restored after the game's `longjmp`.
4. `tools/direct_calls.py`: direct calls between the game functions that have no hook.
5. `tools/localize.py`: local-register copies of the hot leaf functions listed in `games/bo2/locals.txt`.
6. `tools/guest_barriers.py`: compiler barriers for plain guest memory.
7. `tools/codegen_stats.py`: a summary of the generated code.

Always use `codegen.sh`. A bare `rexglue codegen` silently drops the fix-ups, and the builds check for them and stop.
If the generator reports a branch to a target outside any function, `tools/codegen_fix_unresolved.py` declares the
missing functions in `games/bo2/overrides.toml`.

## Step 5: build the shader library

The native renderer draws with SPIR-V translated from the game's own Xenos shaders. The library is built once, from
your files, and is game-derived: it stays in `$BO2_DATA/shaders/` and is never committed.

1. **Extract the shaders** from the game's fastfiles: the Zombies zones of the disc and of the title update, and
   every zone of the DLC packs if you have them.

   ```bash
   python -I tools/inspect/ff_shaders.py <the zones' .ff files> --dump --out "$BO2_DATA/shaders/zones"
   ```

   The Zombies zones are the fastfiles in `game/` and `update/` with a `zm` part in the name (`zm_*`, `*_zm`,
   `so_*_zm_*`), plus `frontend_patch`, `patch` and `weapon_attach_shared`, and every `.ff` under `content/`. The
   fastfiles are encrypted; the tool rebuilds the key from your own decrypted image (step 3), the same way the game
   does, so no key is stored anywhere in the repository.

   Each zone's blobs go to `<zone>_shaders/`, named by content: moving every `.bin` into one folder,
   `$BO2_DATA/shaders/unique`, drops the duplicates.

2. **Translate and compile** each shader: Xenos microcode to HLSL with the XenosRecomp-based translator, HLSL to
   SPIR-V with `dxc`, then `spirv-val`:

   ```bash
   tools/xenosrecomp/run.sh "$BO2_DATA/shaders/unique" "$BO2_DATA/shaders/spirv"
   ```

   With the DLC maps the full set is 16,149 shaders (799 vertex, 15,350 pixel); every one translates and validates.
   It takes about a quarter of an hour on a 16-thread PC.

3. **Pack** them into the library the renderer loads:

   ```bash
   clang++ -std=c++23 -O2 -w -Igames/bo2/src/native/render -Isdk/thirdparty/xxHash \
     tools/xenosrecomp/bo2zm_pack.cpp games/bo2/src/native/render/bo2zm_shader_library.cpp \
     games/bo2/src/native/render/bo2zm_shader_index.cpp -o work/xenosrecomp/bo2zm_pack.exe
   work/xenosrecomp/bo2zm_pack.exe "$BO2_DATA/shaders/unique" "$BO2_DATA/shaders/spirv/spirv" "$BO2_DATA/shaders/bo2zm_shaders.spvpack"
   work/xenosrecomp/bo2zm_pack.exe --index "$BO2_DATA/shaders/bo2zm_shaders.spvpack"
   ```

   The packer uses the renderer's own library code and looks every container up again afterwards. The result is
   about 1.1 GB with the DLC maps. `--index` writes the small index the game reads at start
   (`bo2zm_shaders.spvidx`, next to the pack; copy both). Without it the game makes the index on its first start,
   behind a progress screen (about a minute on a console), and from then on reads shaders on demand.

4. **Multiplayer** draws with shaders of its own. `tools/build_mp_shaders.py` reads the Multiplayer and shared zones
   from `game/`, `update/` and `content/`, translates only the shaders the Zombies library lacks, and writes a pack
   that extends it (`--base-pack`). Its `--help` lists the options.

## The Windows build

```bash
games/bo2/build_windows.sh                             # games/bo2/out/win/bo2zm.exe (clang, Direct3D 12 and Vulkan back ends)
games/bo2/run_windows.sh 0 --bo2zm_renderer=native     # the native renderer; needs the shader library next to the exe
games/bo2/run_windows.sh 0 --bo2zm_renderer=xenos      # exact Xenos emulation, the reference image
```

`run_windows.sh` runs the game against `$BO2_DATA/game` and `$BO2_DATA/update`, logs to `games/bo2/logs/` and allows one
copy at a time. Copy or hard-link `bo2zm_shaders.spvpack` next to the exe. `games/bo2/play_windows.cmd` plays in a normal
window. Saves and DLC live in `Documents\bo2zm\`.

## Linux ARM64

See [building-linux-arm64.md](building-linux-arm64.md). It builds, cross-compiled or native, but is not yet verified
on ARM hardware.

## Campaign

The campaign lives in `default.xex`, a separate program (`bo2cp`) built from `campaign/` with the same renderer and
native code. `campaign/README.md` lists its steps: code generation, its own shader pack, and the Windows
build. Both programs share the game files, the profile and the saves.

## The release kits

`tools/install/` holds the scripts that make the release kits and the installer a kit runs. The installer performs the
steps on this page in order, records its progress so a second run resumes, and clears the developer variables that
change a build, so a checkout builds what a kit builds.

## When something goes wrong

- **"a hooked function is called directly" or "setjmp site lacks the fix"**: run `tools/codegen.sh` again.
- **"refusing to write game-derived output inside the repo"**: write it under `$BO2_DATA`.
- **An LTO link error about symbols used only from inline assembly**: mark them `__attribute__((used))`.
- **"WARNING: building with a stale PGO profile"**: retrain it; a stale profile only loses the gain.
- **A DLC map draws black**: the shader library lacks the DLC zones; rebuild it with them (step 5).
