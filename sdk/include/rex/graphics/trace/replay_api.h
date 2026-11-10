/**
 * C ABI for offline replay of a .xtr GPU trace through a GPU plugin.
 *
 * A capture-enabled plugin (built with REXGLUE_BUILD_GPUTEST=ON) exports
 * rex_gpu_trace_replay. The host (tools/gputest/replay/rex-gpu-replay) creates
 * the graphics system through the normal plugin entry point, sets up a
 * headless Runtime with it, then calls rex_gpu_trace_replay with the object
 * returned by rex_gpu_create. Any plugin that drives the shared
 * rex::graphics::CommandProcessor can implement this entry point with
 * src/graphics/trace/frame_replay.cpp.
 */

#pragma once

#include <cstdint>

namespace rex::system {
class IGraphicsSystem;
}  // namespace rex::system

extern "C" {

enum RexGpuTraceReplayFlags : uint32_t {
  // Read back every bound color render target after each draw.
  kRexGpuTraceReplayReadbackColor = 1u << 0,
  // Read back the depth buffer after each draw that has depth or stencil on.
  kRexGpuTraceReplayReadbackDepth = 1u << 1,
  // Write the full image for every draw instead of the changed rectangle.
  kRexGpuTraceReplayFullImages = 1u << 2,
  // Check every bound texture against the guest bytes it was loaded from and
  // write texcheck.jsonl (stale texture report).
  kRexGpuTraceReplayTexCheck = 1u << 3,
  // Race mode (implies TexCheck): apply each recorded guest write that hits a
  // texture inside that texture's load window, after the copy and before the
  // cache marks it up to date.
  kRexGpuTraceReplayRace = 1u << 4,
};

struct RexGpuTraceReplayOptions {
  uint32_t struct_size;
  const char* trace_path;
  const char* output_dir;
  // Free form label written to summary.json (e.g. "vulkan-moltenvk").
  const char* backend_label;
  uint32_t flags;
  // Stop after this many frames (swap events); 0 = whole trace.
  uint32_t max_frames;
  // Guest physical address of a scratch region (at least 32 MiB) that the
  // trace does not use, for readback resolves. 0 = pick automatically.
  uint32_t scratch_address;
  // Only read back draws in [first_draw, first_draw + draw_count); count 0 =
  // all draws. Draws outside still execute and get JSON records.
  uint32_t first_draw;
  uint32_t draw_count;
  // Race mode: how many frames ahead of the current one a recorded write may
  // be pulled into a load window. 0 = default (1).
  uint32_t race_frames;
};

// Returns 0 on success, nonzero on failure.
typedef int (*PFN_rex_gpu_trace_replay)(rex::system::IGraphicsSystem* graphics_system,
                                        const RexGpuTraceReplayOptions* options);

}  // extern "C"
