# Technical documentation

How bo2-360-nx works, what was optimized and what it gained, and where to go to build it.

| Page | What it covers |
|---|---|
| This page | The pipeline, the runtime, the renderer, networking, the optimizations, testing |
| [building.md](building.md) | Building from source on Windows: tools, every step, build options, PGO, the SD card layout |
| [building-linux-arm64.md](building-linux-arm64.md) | The Linux ARM64 build (it builds; not yet verified on ARM hardware) |
| [features.md](features.md) | Every menu page and option, and the settings file |
| [script-mods.md](script-mods.md) | The GSC Loader: mod folders, `mod.ini`, startup scripts, limits |

Elsewhere: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists every upstream project and its license;
`games/bo2/README.md`, `sdk/README.md`, `tools/README.md` and `patches/README.md` describe their folders.

## Contents

[The pipeline](#the-pipeline) · [Code generation](#code-generation) · [Hooks](#hooks) · [The runtime](#the-runtime) ·
[The native renderer](#the-native-renderer) · [Networking](#networking) · [The game's own systems](#the-games-own-systems) ·
[Optimizations](#optimizations) · [Testing and measuring](#testing-and-measuring)

## The pipeline

The Xbox 360's PowerPC code is translated ahead of time into C++, compiled for the target's CPU and linked with a
runtime that provides what the 360's kernel and hardware did. The GPU is not emulated: a renderer of its own reads the
game's GPU commands and draws them with Vulkan.

```
 your disc + TU18            build time (PC)                          run time (Switch)
 default_mp.xex  ──▶  loader: decrypt, decompress, patch       bo2zm.nro
 default_mp.xexp      code generator: 30,096 functions   ──▶    ├ recompiled game code
                      fix-up passes → C++ → GCC 16 (LTO, PGO)   ├ runtime: kernel, memory, threads, files, audio
 fastfiles (.ff) ──▶  shaders → XenosRecomp → dxc        ──▶    ├ native renderer → Vulkan (NVK)
                      → bo2zm_shaders.spvpack                   └ Switch layer: threads, cores, display
```

Zombies and Multiplayer live in `default_mp.xex`; the Campaign is `default.xex`, recompiled the same way as a second
program (`bo2cp`) that shares the renderer and native code. Both target Title Update 18, the game's final update.

**No Direct3D translation.** On the Xbox 360 the game's Direct3D layer is part of the game itself and drives the Xenos
GPU directly with PM4 command packets and shader microcode. That layer is recompiled like the rest of the game; the
renderer reads the stream it writes and calls Vulkan itself: NVK on the Switch, the system's driver on a PC. On the PC
the SDK's exact Xenos emulation (Direct3D 12, `--bo2zm_renderer=xenos`) remains available as the reference image.

## Code generation

The SDK's loader (from Xenia) decrypts the XEX2 image, decompresses it and applies the update's delta patch: a 47 MB
image at 0x82000000 that imports 162 functions from `xam` and 215 from `xboxkrnl`. ReXGlue's code generator
(`sdk/src/codegen`, driven by `games/bo2/bo2zm_manifest.toml`) then:

1. **Analyzes** the image from the entry point, exports, call targets, jump tables and exception tables until nothing
   new appears: 30,096 functions, no errors.
2. **Emits** one C++ function per PowerPC function, about 5 million lines in 140 files. Guest memory is one base
   pointer with byte swaps; VMX128 maps to SIMDe (NEON on ARM, SSE on x86).

`games/bo2/overrides.toml` holds what the analysis cannot find (boundaries, tail-branch thunks, `setjmp` addresses, register
options), each entry with its evidence; `games/bo2/gaps.toml` lists code no function covers.

**Registers as locals.** The non-volatile general, floating-point and vector registers, the condition registers, CTR,
XER and the scratch vector registers are C++ locals, so the compiler keeps them in host registers. Only argument and
volatile registers stay in the context structure.

**Fix-up passes**, run in order by `tools/codegen.sh`:

| Pass | Why |
|---|---|
| `read_before_write.py --strict` | Finds functions that read a register before writing it; they must share registers |
| `setjmp_locals.py --fix` | `longjmp` error recovery must restore the register locals |
| `direct_calls.py` | Direct calls to unhooked functions, so the compiler can inline them |
| `localize.py` | Local-register copies of the hot leaf functions in `games/bo2/locals.txt` |
| `guest_barriers.py` | Compiler barriers that keep plain guest memory accesses correct |

GCC 16 (devkitA64) compiles the generated code, the SDK and the port at `-O3` with LTO and PGO into one NRO. Switch
code is behind `__SWITCH__`; the same sources build for Windows and Linux with clang.

## Hooks

Each generated function is `__imp__sub_<address>`, called through a weak alias `sub_<address>`. A hook is a C++
function of that name, which the linker prefers; it can still call the original. Hooks replace hot functions with
native rewrites, observe the game (its Direct3D layer, the lighting volume's writer, the job system) and add features
(menus, cheats, presets, networking, the GSC Loader).

**Exactness.** Every rewrite of game code first runs beside the original and compares their outputs word for word,
then runs alone with a sample still checked; a difference turns it off until the game restarts. Caches check a sample
of their hits the same way.

## The runtime

The ReXGlue SDK (`sdk/`) carries Xenia's implementation of the 360's kernel and libraries, plus a Horizon layer.

- **Kernel**: the 377 imports (threads, events, semaphores, mutants, memory, files, content, sockets, profiles,
  system UI). On Horizon, sync objects wait on the address arbiter, so a signal never takes a lock.
- **Memory**: Horizon has no sparse memory and cannot change permissions on mirrored memory, so the guest window is
  reserved empty, committed in chunks and mapped into every view on first access; the exception handler emulates
  accesses to unmapped pages (`sdk/src/core/guest_memory_switch.h`).
- **Threads**: each guest thread is a Horizon thread. The game's job system is uses the SDK spin-wait backoff (`sdk/src/core/thread_spin.cpp`,
  cvar `spin_wait_backoff`): a guest thread that polls back to back sleeps a growing time instead of spinning.
- **Fourth core**: from a four-core forwarder, the game wraps the kernel's core-mask and priority calls and places
  threads by rule (the Server on all four cores, the mixer and XMA decoder alone on core 3). A failsafe returns to
  three cores when core 3 stays busy for two seconds (`sdk/src/ui/switch_core3.cpp`).
- **Audio**: the game mixes on its own threads; the runtime provides XAudio and the XMA decoder (FFmpeg, with a frame
  cache), out through `audout`.
- **Files and DLC**: the game, update and `user` folders; DLC packages are mounted as the 360 did. Image pack reads are
  paced, because at memory speed the game's streaming re-reads the same images endlessly.
- **Input**: HID controllers presented as Xbox 360 controllers, up to four, each with an offline profile.
- **Display**: the 1280x720 image scaled to the screen, VSYNC on the display's refresh signal, docked and handheld.
- **Logs**: written by their own thread, so logging never waits for the SD card.

## The native renderer

The renderer's thread reads the PM4 packets the game writes, keeps the register state, and for every draw identifies
the shaders by hash in the shader library, finds the Vulkan pipeline, resolves the textures (untiled and byte-swapped
into the texture pool), uploads vertices, indices and the constants the shaders read, and records the draw. Resolves,
fences, memory waits, occlusion queries and memexport are handled the same way.

- **Write stamps**: guest memory cannot be made read-only on Horizon, so each 4 KB page carries an epoch advanced by
  the code that writes it (copy and fill routines, skinning hooks, memexport). Cached vertex data, indices and textures
  stay valid while their pages are unchanged.
- **Shader library**: every shader is translated at build time (XenosRecomp, then `dxc`) into one pack, 16,149 shaders
  for Zombies with the DLC maps, more for Multiplayer and the Campaign. An index built on the first start lets each
  shader be read on first use. Pipelines are compiled on a thread, cached, and precompiled at start after an update or
  a driver change.
- **Threads**: a recorder thread makes the Vulkan calls the ring thread queues; others copy vertex data and bind
  texture memory.
- **Texture pool**: large blocks with one GPU mapping per block and memory kind, evicted in small batches.
- **GPU skinning**: the game's CPU skinning jobs become compute dispatches.
- **Driver**: NVK, built for Horizon by the mesa-switch project, with this project's patch (`patches/`: state
  fingerprints, copy-engine copies, kind views, a leak fix).
- **Split screen**: each view is drawn by the game as on the 360; script mod HUDs are drawn through a full-screen
  placement scaled into their owner's view.

## Networking

**System Link.** BO2 finds hosts by a broadcast from its UDP port 1001; the host answers with its XNADDR, and the
joiner connects to that address on port 1000. The runtime gives each console an XNADDR built from its LAN address, a
per-console XUID, and moves ports below 1024 up by 30000, so the game uses UDP 31000 and 31001. A side socket on UDP
31002 carries the port's cheat settings from joiners to the host, which applies them to their players, and the host's
word on which HUD elements are a script mod's menu, so a joining console fits them into its split-screen views.

**Internet play.** A join code is 8 Crockford base32 characters: the host's IPv4 address and a CRC-8. Joining runs an
ordinary System Link game over the internet:

1. **UPnP** asks the router to forward the ports. Some routers accept the request and forward nothing, so the game
   does not stop there.
2. **The relay**: both sides learn their public endpoints from STUN servers, using the game's own sockets, and swap
   them through free public MQTT brokers, several at once (`net_online_brokers`). The brokers pass only those
   addresses.
3. **Hole punching**: both sides send punch datagrams to each other's endpoints, which opens a path through routers
   and carrier NATs that keep a port's mapping (endpoint-independent NAT). The socket layer drops these datagrams
   before the game sees them.
4. **Manual forwarding** of UDP 31000-31002 to the host stays the last resort.

The code is in `sdk/src/system/online_relay.cpp`, `online_link.cpp` and `xsocket.cpp`, and `games/bo2/src/bo2zm_online.cpp`.

## The game's own systems

- **Menus** are HavokScript (Lua 5.1) in the fastfiles; the game's own Lua compiler builds `games/bo2/src/ui/menus.lua` at
  start to add the port's pages. `tools/inspect/hks_dis.py` disassembles the game's menus.
- **Settings**: presets set the game's own dvars, and hooks hold the ones it forces back every frame.
- **Cheats and Extras** drive the game's script engine (notifies, function calls, field reads) on the Server thread,
  right after its frame, where the game runs its own scripts.
- **The GSC Loader** replaces script assets in memory before the game links them: each server load gets a fresh
  writable copy, so original game files are never touched.

## Optimizations

These are the changes that stayed, by area, with what each gained. Percentages are frame-rate gains on a console unless marked; most come from an A/B inside
one benchmark run, where a factor that changes nothing shows the noise (1-2 %). With every core busy, time one thread
saves often goes to another, so some changes show only in per-thread CPU.

**Code generation**
- Plain guest memory: non-volatile loads and stores, kept correct by compiler barriers. Code 7.2 % smaller, Server
  thread 6-9 % less CPU; **+5.8 %** together with GPU skinning, the limiter and the entity loops.
- Registers as C++ locals: all 31,863 save/restore helper calls gone. Part of the first pass of changes.
- Scratch vector registers as locals: **+1.7 %**.
- Direct calls: 97 % of calls between game functions are direct and can be inlined; the base for LTO.
- Native `memcpy`, `memmove` and `memset` at all 4,190 call sites.

**Compiler**
- Link-time optimization across the 140 generated files: **+9.8 %**.
- Profile-guided optimization on Horizon, trained on the benchmark and checked by `tools/pgo_check.py`: **+9.0 %** when
  retrained, **+8.2 %** on four cores; the port's own code trained too, **+1.8 %**.

**Native rewrites of game code**
- Fastfile stream decoder: **1.9x** per call.
- Bounds-tree culling, same SIMD operations in the same order: **2.0x** per call.
- CPU skinning kernels, bit-exact: **1.4-1.7x** per call.
- DLC loading videos through FFmpeg's NEON VP8 decoder: the game runs at full speed while a video plays, where it ran at about a third.
- Stream allocator search, remembered: per search **6-8x** faster (PC).
- Entity scan **-10.6 %** time; plane test **-33 %**.
- Four Direct3D functions in native code: no measurable gain, kept because exact.

**Threads and waits**
- The fourth CPU core: **+16 %** on a Switch Lite, **+11.9 %** on an OLED.
- Blocking job-system waits: 1.3-1.7 cores less CPU at the same frame times (PC).
- Kernel objects on the address arbiter: a contended clock read takes 56 % less time, and is exact.
- Job sleepers on their own word: lock waits halved.
- Sleeping instead of putting blocked jobs back: **+1.2 %**.
- The log on its own thread: **+1.0 %**, and the worst frame 5x shorter.
- GPU skinning job store, sharded: job workers' lock waits from 14-16 % to 1-2 % of samples.

**The renderer**
- The first set (sampler cache, lighting volume by pages, audio off the renderer's core, NVK state fingerprints,
  copy-engine uploads): per-draw cost more than halved, **+70-80 %** frame rate.
- Write stamps and the vertex cache: **+5.4 %**.
- Texture rechecks by write stamps: **+2.3 %**, then **+0.4 %**.
- Constants by the registers each shader reads: **+2.2 %**.
- Texture eviction by key: **+1.9 %**.
- Index cache: **+1.1 %**. Lighting volume by its writer: **+1.0 %**.
- Register dumps decoded in blocks: state decoding 23-27 % cheaper (PC).
- Deferred recording on a second thread; upload deduplication (**-23 %** per draw, PC); texture words reused (65-70 %
  of draws skip the sampler loop); vertex inputs by generation (**+0.5 %**).

**The GPU and the driver**
- Image copies on the copy engine: **+9.5 %**.
- Pipeline state by fingerprints: 86-89 % of the state skipped on a bind.
- Kind views in NVK: map calls cut to a third, **+0.4 %**.
- Skinning on the GPU: job workers 7 % of a core lighter.

**Memory and streaming**
- Shader library by index: ready **1,400x** faster at start; launch to match more than halved.
- Image streaming read rate paced like a disc: **+4.3 %**.
- Texture pool sized for its 64 KB units: no texture sent to the slow path.
- SPIR-V not kept in memory: about 260 MB of heap back. Leaks closed: heap flat over an hour.

**Audio**
- Decoded XMA frame cache: **+3.8 %**.
- Sound mixer rewrites (reverb, resamplers, biquad, mixing): **+2.7 %**.
- The limiter in one pass: **1.4x** per call.

**Loading and hitches**
- Pipelines compiled on a thread: preset changes no longer freeze the game for seconds.
- Shader precompile at start: a first visit compiles about 95 % fewer pipelines in the match.
- Pipeline cache saved in slices, outside matches; the sound bank wait polled at 1 ms (PC: first globe frames 7x
  faster).

**Presets**
- LOWEST: **+10.7 %** over very low. Very low: half the draws of the game's own settings.
- FAR OBJECTS: SIMPLE: 15-23 % fewer draws than a full draw distance on Buried.
- Model detail applied where the game picks a model's version: Buried's cave walls fixed at no cost.

**Tried and not adopted**: a full Direct3D-level renderer (about +2 % at best), GPU reads of guest memory in place
(refused by Horizon), a hashing vertex cache, texture hash sampling, thread priorities (-2 to -5.4 %), flush-to-zero
scalar math (-3.6 %), volatile registers as locals (+0.07 %), a wider NEON copy, NEON four-plane culling (1.7x per call,
no frame-rate change), early prefetching, longer socket idle windows, C heap tweaks, a 128 MB XMA cache and an internal
resolution scale (about 10 % of the GPU's frame, no gain while the CPU sets the pace).

## Testing and measuring

The tools are part of the normal build and off by default; a test is a `bo2zm.toml` with test settings added.

- **The benchmark** (`bo2zm_benchmark`) starts a match, turns the player in place so every run sees the same scene,
  and writes one summary line: average frame rate, percentiles and the worst frame. `bo2zm_benchmark_round` starts
  at a later round.
- **A/B arms** (`bo2zm_benchmark_ab_s`) alternate up to two settings every few seconds within one run, with frame
  times and each thread's CPU per arm. Changes that defer work look better in short arms than they are; those, and
  changes of 1-2 %, are judged on alternating whole runs.
- **The profiler** writes each thread's CPU, core mask, system calls and GPU time to `logs/rex/rex_profile.log` every
  10 seconds, with optional stack samples (`tools/profile_sym.py` names them from the unstripped ELF).
- **Self-checks**: every native rewrite and cache compares itself with the real result; a difference logs `MISMATCH`
  and turns that piece off. The D3D shadow (`bo2zm_native_d3d_shadow`) rebuilds a sample of draws from the game's
  Direct3D side and compares them word for word with the renderer's.
- **The Xenos reference** on the PC (`--bo2zm_renderer=xenos`) is the image every renderer change is checked against.
- **Rules**: compare runs only at the same clocks, preset, CPU cores, FPS cap and scene. The PC is for correctness;
  speed counts only on a console.
