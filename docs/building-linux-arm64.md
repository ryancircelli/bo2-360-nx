# Building for Linux ARM64 (AArch64)

The same game as the Windows build (recompiled code, the `sdk/` runtime, the native Vulkan renderer), with SDL3 for
the window, input and audio, for 64-bit ARM Linux desktops with a Vulkan GPU. It contains code recompiled from your
copy of the game: keep it for your own machines.

**Status: it builds, but has not run on real ARM hardware yet.** Cross-compiling on x86_64 is the tested path; a
native build on the ARM machine uses the same script.

## Before you start

- **Generated C++** in `app/generated/`, from `tools/codegen.sh` (it runs on Windows; the output is the same for every
  platform).
- **The SDK's third-party sources**: `python3 tools/fetch_thirdparty.py`.
- Your game files and shader library are not needed to build; they go next to the program afterwards.

## Cross-compiling on x86_64 (Ubuntu 24.04 or WSL2)

Clang 19 or newer is required (with clang 18, libstdc++'s `<expected>` is empty and the SDK does not compile):

```bash
sudo apt install clang-20 lld-20 llvm-20 cmake ninja-build pkg-config python3 libwayland-bin mmdebstrap
sudo bash tools/linux/make_sysroot.sh     # out/linux-sysroot, ~460 MB of unpacked Ubuntu 24.04 ARM64 packages
bash app/build_linux.sh                   # app/out/linux-arm64/package/bo2zm-linux-arm64/ and a .tar.gz
```

A first build took 11 minutes in WSL2 on a 16-thread PC (12 jobs in 15 GB; lower `JOBS=` on smaller machines). In
WSL2, keep the sysroot and build folder on the Linux file system:

```bash
sudo bash tools/linux/make_sysroot.sh ~/bo2zm/sysroot
BO2_LINUX_SYSROOT=~/bo2zm/sysroot BO2_LINUX_OUT=~/bo2zm/build bash app/build_linux.sh
```

**Docker** (`tools/linux/Dockerfile`, the same environment with the sysroot built in; not yet run), from the top of
the checkout:

```bash
docker build -t bo2zm-linux-arm64 -f tools/linux/Dockerfile tools/linux
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" bo2zm-linux-arm64
```

## Building on the ARM64 machine (not yet tried)

```bash
sudo apt install clang lld llvm cmake ninja-build pkg-config python3 libwayland-bin \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
  libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev
bash app/build_linux.sh
```

On Ubuntu use `clang-20 lld-20`. The binary then needs the build machine's glibc or newer; set `BO2_LINUX_SYSROOT`
for the same binary as a cross build.

## Generating the C++ on Linux

`rexglue` builds and runs on Linux (x86_64 and ARM64), so the whole chain can run there:

```bash
python3 tools/fetch_thirdparty.py
bash tools/linux/build_host.sh                # rexglue and xexdump, about 2 minutes on 16 threads
export REXGLUE_BIN_DIR=$PWD/sdk/out/linux-arm64 LD_LIBRARY_PATH=$PWD/sdk/out/linux-arm64 PYTHON=python3
# (sdk/out/linux-x86_64 on an x86_64 machine; the build script prints the path)
bash tools/codegen.sh
```

Steps 1 and 3 of docs/building.md (game files, the decrypted image) need nothing else: `tools/install/xiso.py` reads the
disc image and `tools/inspect/stfs_extract.py` the title update. The shader library needs `dxc` and `spirv-val`: on
Ubuntu 24.04 arm64 `spirv-tools` is packaged but `dxc` is not, so build it from the DirectXShaderCompiler sources
(about 25 minutes) and give `tools/xenosrecomp/run.sh` the paths with `DXC=` and `VAL=` (plus `LD_LIBRARY_PATH` for
`libdxcompiler.so`).

## Build options

| Variable | Default | What it does |
|---|---|---|
| `BO2_LINUX_SYSROOT` | `out/linux-sysroot` | The sysroot; native builds use one only when it is set |
| `BO2_LINUX_OUT` | `app/out/linux-arm64` | The build folder |
| `BO2_LINUX_MARCH` | `armv8-a` | `-march`; `armv8.2-a` suits Cortex-A76/A78, Neoverse N1 and newer |
| `BO2_GUEST_MEMORY_PLAIN` | `1` | Plain guest memory accesses, as on the Switch; `0` is the volatile form |
| `BO2_LTO` | `0` | `1` turns on ThinLTO |
| `CLANG_SUFFIX` | first clang ≥ 19 | A versioned clang, e.g. `-20` |
| `JOBS` | CPU count, at most 12 | Parallel jobs |

Release (`-O3`), with libstdc++ and libgcc linked statically.

## The package

```
bo2zm-linux-arm64/
  bo2zm                 the game
  librexruntime.so      the SDK's runtime
  librexgpu-xenos.so    the emulated Xenos GPU (only for --bo2zm_renderer=xenos --gpu_plugin=xenos)
  bo2zm.sh              the launcher
  bo2zm.toml            the settings
  README.txt
```

Next to `bo2zm.sh` go `game/` (the disc's files plus TU18's `default_mp.xexp`), `update/` (the rest of the title
update) and the shader library (`bo2zm_shaders.spvpack` and `.spvidx`, the same files as the Windows build's). Start
with `./bo2zm.sh`; extra arguments pass through (`./bo2zm.sh --fullscreen=false`). Settings come from `bo2zm.toml`
beside it. Saves, caches and the DLC packs (`user/0000000000000000/`, as on the Switch) go to a `user/` folder next to
the launcher when there is one, otherwise to `$XDG_DATA_HOME/bo2zm` (usually `~/.local/share/bo2zm`).

The target needs glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or later), 4 KB memory pages, an X11 or
Wayland desktop, the Vulkan loader with a driver, and PipeWire, PulseAudio or ALSA.

## Vulkan drivers

The SDK requires `independentBlend`, `fragmentStoresAndAtomics` and `vertexPipelineStoresAndAtomics`, and by default
`geometryShader` and `fillModeNonSolid`, which the native renderer never uses: on a GPU without them, start with
`--vulkan_require_geometry_shader=false --vulkan_require_fill_mode_non_solid=false`.

| GPU | Driver | Expected |
|---|---|---|
| Qualcomm Adreno 6xx/7xx/X1 | Mesa Turnip | Should work |
| NVIDIA (Jetson Orin, Grace, workstation GPUs) | NVIDIA | Should work |
| AMD Radeon in ARM workstations | Mesa RADV | Should work |
| Raspberry Pi 4/5 (VideoCore VI/VII) | Mesa V3DV | Probably works, far slower than the Switch; the Pi 5 needs a 4 KB kernel |
| Arm Mali (RK3588 and similar) | Mesa PanVK | Unknown; no geometry shaders, so use the options above |
| Apple M1/M2 (Asahi Linux) | Mesa Honeykrisp | Blocked: 16 KB pages only |
| Any | Mesa lavapipe (CPU) | Runs, far too slowly to play |

None has been tried on real hardware.

## Known limits

- **4 KB pages only**: the 360's memory layout assumes them, so the program stops at start on bigger pages. On a
  Raspberry Pi 5, add `kernel=kernel8.img` to `/boot/firmware/config.txt`.
- **glibc 2.38**: Ubuntu 22.04, Debian 12 and L4T are too old.
- **No console mode**: an X11 or Wayland desktop is required.
- **Code generation on Linux**: see the next section. The generated C++ is the same on every platform.

## What has been verified

Built under WSL2 (Ubuntu 24.04, clang 20.1.2) against the sysroot: the game, runtime, Xenos plugin and SDL (X11 and
Wayland) compile and link, and the package needs only glibc 2.38 and the X11, xcb and Wayland client libraries. Under
QEMU 10 user-mode emulation (`tools/linux/smoke_qemu.sh`, lavapipe, a virtual X server) the game starts, applies the
title update, opens Vulkan and audio, and draws the Zombies main menu for ten minutes without an error (QEMU 8.2
itself crashes after a minute or two). Matches, performance and real drivers are not checked yet.

## Reproducing the build

The sysroot comes from Ubuntu 24.04's release pocket only, which never changes; `tools/linux/sysroot-packages.txt`
lists its versions, and `make_sysroot.sh` warns on a difference. The compiler (`clang-20`, tested at 20.1.2) is the
one moving input. The build ID includes the build time, so builds are not bit-identical.
