# GPU frame capture and offline replay diff

Find the exact draw and shader where a GPU backend goes wrong, without playing
the game: capture a frame of the PM4 command stream once, replay it offline on
two backends, read back the render targets after every draw, and diff the two
replays draw by draw.

Everything here is built only with `-DREXGLUE_BUILD_GPUTEST=ON` (default OFF).

## Pieces

| Piece | Where |
| --- | --- |
| Trace format (Xenia `.xtr` v1) | `include/rex/graphics/trace/trace_protocol.h` |
| Command processor hooks | `include/rex/graphics/trace/trace_hooks.h`, calls in `src/graphics/command_processor.cpp` (null pointer check, inert unless a hook object is installed) |
| Capture (writer + env trigger) | `src/graphics/trace/trace_writer.*`, `src/graphics/trace/frame_capture.*` |
| Replay (player + per-draw readback) | `src/graphics/trace/frame_replay.*`, exported from the GPU plugin as `rex_gpu_trace_replay` (`include/rex/graphics/trace/replay_api.h`) |
| Replay tool | `tools/gputest/replay/replay_main.cpp` -> `rexgpu-replay` |
| Diff | `tools/gputest/replay/replay_diff.py` (numpy + Pillow) |
| Trace dump | `tools/gputest/replay/xtr_dump.py` |
| Synthetic test trace | `tools/gputest/replay/make_synthetic_trace.py` |
| Metal plugin patch | `tools/gputest/replay/patches/rexgpu-metal-gputest.patch` |

With `REXGLUE_BUILD_GPUTEST=ON` the trace sources are compiled into
`rexgpu-xenos` (and into `rexgpu-metal` with the patch) and `REX_GPU_TRACE=1`
is defined for them.

## Build

```sh
cmake --preset mac-arm64 -DREXGLUE_BUILD_GPUTEST=ON
cmake --build out/build/mac-arm64 --config Release --target rexgpu-replay
# -> out/mac-arm64/Release/rexgpu-replay + librexgpu-xenos.dylib next to it
python3 -m venv venv && venv/bin/pip install numpy pillow   # for the diff
```

## Capture

The capture is in the GPU plugin's command processor, so any game that loads a
`REXGLUE_BUILD_GPUTEST` plugin can record. Environment variables:

| Variable | Meaning |
| --- | --- |
| `REX_GPU_TRACE_FRAME=N` | record the frame after the N-th swap (0 and 1 both mean the first full frame) |
| `REX_GPU_TRACE_FRAMES=K` | record K consecutive frames (default 1) |
| `REX_GPU_TRACE_DIR=dir` | output directory (default `gpu_traces`) |
| `REX_GPU_TRACE_TRIGGER=path` | poll this file at every swap; when it appears, record (the file may hold a frame count) and rename it to `path.done`. Use `touch path` from another shell to grab whatever is on screen. |

Output: `<dir>/<title id %08X>_<swap number>.xtr`.

Run the game with `readback_resolve = "full"` while capturing if it reads
resolved surfaces back on the CPU, so guest memory holds the resolved data
at capture time.

What is recorded at trace start: all 0x5003 registers, the gamma ramp, and the
microcode of the bound vertex and pixel shaders (as synthetic
`IM_LOAD_IMMEDIATE` packets at physical `0x1FFC0000`). During the frame: every
PM4 packet (primary ring and indirect buffers, with start/end markers), the
memory the GPU reads (ALU constant loads, shader loads, wait polls, and at each
draw the index buffer, every vertex fetch range the vertex shader uses and the
base and mip data of every bound texture), and a swap event per frame. Memory
ranges are deduplicated by content hash, and ranges written by a resolve
earlier in the frame are not re-captured (replay produces them itself).

## Replay

```sh
rexgpu-replay capture.xtr -o replays/vulkan --label mvk
rexgpu-replay capture.xtr -o replays/metal --plugin /path/to/librexgpu-metal.dylib --backend any --label metal
```

Options: `--plugin path|name` (name `xenos` loads `librexgpu-xenos` next to the
tool), `--backend vulkan|any`, `--no-color`, `--no-depth`, `--full` (write full
images for every draw instead of the changed rectangle), `--frames N`,
`--draws FIRST:COUNT` (read back only this window), `--scratch 0xADDR`,
`--cvar name=value`, `--log-level`.

The replay creates a headless runtime (no game, no kernel), loads the plugin,
maps all of physical memory, and on the command processor thread executes the
trace: memory reads are written to guest memory (and the GPU caches told), leaf
packets are executed with `ExecutePacket`, indirect buffer packets are not
re-executed (their packets are in the trace). `WAIT_REG_MEM` returns at once
during replay. Replay forces `readback_resolve = full` and
`async_shader_compilation = false` (otherwise draws are skipped while their
pipeline compiles).

Per-draw readback: after each draw, for each color target the pixel shader
writes (and the color mask enables) and for depth when depth or stencil test is
on, the replayer issues its own resolve through the backend (register writes
plus a `DRAW_INDX_2` rect list in a scratch area, default physical
`0x1C000000`, 32 MiB), reads it from guest memory, untiles it, and restores
every register it touched. Color is resolved to `k_32_32_32_32_FLOAT`, depth
raw (24-bit value in the top bits, D24S8 or D24FS8). The readback uses the
backend's own resolve path, so a broken resolve shows up as a divergence too.

Output directory:

* `draws.jsonl`: one JSON object per draw: `draw`, `frame`, `packet`,
  `prim_type`, `source_select`, `index_count`, `indexed`, `index_format`,
  `index_base`, `index_bytes`, `succeeded`, `edram_mode`, `is_resolve`, `vs`,
  `ps` (ucode hash `%016X`, the same hash the SDK uses in shader storage and
  logs), `regs` (RB_COLORCONTROL, RB_DEPTHCONTROL, RB_BLENDCONTROL0..3,
  RB_COLOR_MASK, PA_SU_SC_MODE_CNTL, PA_SU_POLY_OFFSET_*, PA_CL_VTE_CNTL,
  RB_SURFACE_INFO, RB_COLOR_INFO 0..3, RB_DEPTH_INFO, viewport scale/offset,
  window and screen scissors, stencil refs, alpha ref and more), `textures`
  (fetch constant index, the 6 raw dwords, format, base), `vertex_fetch`, and
  `targets` (label, size, format, XXH3 of the full image, `full` or `delta`
  file and its rectangle).
* `draws/NNNNNN_<target>.npy`: float32 HxWx4 color or uint32 HxW raw depth.
  Only the rectangle that changed since the previous readback of the same
  target is stored unless `--full`.
* `summary.json`: totals and status.

Exit code is non-zero if the trace could not be replayed.

## Diff

```sh
replay_diff.py diff replays/vulkan replays/metal -o diff_out [--delta]
replay_diff.py png replays/vulkan -o pngs
```

Reports the first divergent draw (its frame, packet, target, differing pixel
count, max error, VS and PS hashes, primitive, counts and every state register
that differs between the two records), writes `report.md`, `report.html`,
`result.json` and side-by-side images `drawNNNNNN_<target>.png` (A, B, diff
heat map). Tolerances: `--tol` (color, default 1.5/255 per channel),
`--depth-tol` (default 16 in 24-bit units), `--max-pixels`. `--delta` compares what each draw changed (after minus before, per backend)
on the pixels either backend wrote, and lists every independent divergence,
so a draw that differs only because an earlier draw did is not reported again
(inherited error cancels exactly for opaque writes and is scaled down by the
blend factor when blending). `result.json` lists every finding with its pixel
count, max error and shader hashes.
`--a-frame/--b-frame` compare one frame of each replay (for traces of
different lengths). Exit code 1 when something diverges.

## Trace file format

Xenia's `.xtr` v1 (BSD, `src/xenia/gpu/trace_protocol.h`), little endian,
packed:

```
TraceHeader { u32 version = 1; char build_commit_sha[40]; u32 title_id; }
then commands, each starting with u32 TraceCommandType:
  0 PrimaryBufferStart  { type, base_ptr, count }
  1 PrimaryBufferEnd    { type }
  2 IndirectBufferStart { type, base_ptr, count }
  3 IndirectBufferEnd   { type }
  4 PacketStart         { type, base_ptr, count } + count * 4 packet bytes (guest byte order)
  5 PacketEnd           { type }
  6 MemoryRead          { type, base_ptr, encoding, encoded_length, decoded_length } + data
  7 MemoryWrite         same as MemoryRead
  8 EdramSnapshot       (not written)
  9 Event               { type, event_type }   (0 = swap)
 10 Registers           { type, first_register, register_count, u8 execute_callbacks, encoding, encoded_length } + data
 11 GammaRamp           { type, u8 rw_component, encoding, encoded_length } + 256 + 3*128 dwords
```

`base_ptr` values are physical addresses. This writer stores data uncompressed
(`encoding = 0`); the reader rejects snappy-compressed traces from Xenia for
now. `build_commit_sha` holds `rexglue-gputest`. Traces should replay in
Xenia's trace viewer too (not tested).

## Testing without a game

```sh
make_synthetic_trace.py tri.xtr              # clear + red, green, blue triangles with depth test
make_synthetic_trace.py tri_bug.xtr --bug    # third triangle magenta
rexgpu-replay tri.xtr -o good && rexgpu-replay tri_bug.xtr -o bad
replay_diff.py diff good bad -o d            # FIRST DIVERGENT DRAW 3 ... rt0
```

Capture round trip (capture runs inside replay too):

```sh
make_synthetic_trace.py two.xtr --frames 2
REX_GPU_TRACE_FRAME=1 REX_GPU_TRACE_DIR=cap rexgpu-replay two.xtr -o orig
rexgpu-replay cap/00000000_1.xtr -o re
replay_diff.py diff orig re --a-frame 1      # no divergence
```

## Reference backend: lavapipe in Linux

Vulkan on Mesa lavapipe (CPU) is the slow but independent reference. Build the
same tree on Linux (`cmake --preset linux-arm64 -DREXGLUE_BUILD_GPUTEST=ON`,
target `rexgpu-replay`) and run with the lavapipe ICD:

```sh
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json rexgpu-replay capture.xtr -o replays/lavapipe
```

Diff its output against the Mac replays (copy the replay directory over; the
format has no host dependency).

Measured noise floor: a bo2 main menu frame (70 draws) replayed on MoltenVK
(Apple GPU) and on lavapipe differs by at most 2/255 on 62k pixels in absolute
mode, and in delta mode with `--tol 0.0118` (3/255) by 4 to 5/255 on 2 to 71
pixels in six textured UI draws (filtering and blend rounding). Use
`--tol 0.02` or `--max-pixels 100` between unrelated GPUs; a real shader or
state bug shows up as large errors on many pixels.

## Using it per renderer

* SDK Xenos plugin (Vulkan, MoltenVK on Mac): build with the option, run the
  game with the gputest `librexgpu-xenos` and `REX_GPU_TRACE_*`, then replay on
  MoltenVK and on lavapipe and diff.
* Metal plugin (`rexgpu-metal`): apply `patches/rexgpu-metal-gputest.patch` to
  the Metal branch. It compiles the trace sources from the plugin's private SDK
  copy and implements `TraceReadbackGuestMemory` (flush, then copy the resolve
  from the shared memory buffer into guest memory). Replay a trace captured
  with any backend through `--plugin librexgpu-metal.dylib --backend any` and
  diff against the Vulkan replay.
* A game's own renderer that reads the PM4 ring itself (bo2's native renderer)
  bypasses the plugin. Capture with the Xenos plugin path of the same game
  (the trace is backend-independent), then make the native renderer replay it
  or emit the same `draws.jsonl` + `.npy` layout so `replay_diff.py` can
  compare it.

## Limits

* Immediate-mode index data (`DRAW_INDX` with immediate indices) and memexport
  writes are replayed as packets but are not specially captured.
* Readback covers 1x MSAA targets fully; with MSAA the resolve picks sample 0.
* Replay is POSIX only for now (the tool uses `dlopen`).
