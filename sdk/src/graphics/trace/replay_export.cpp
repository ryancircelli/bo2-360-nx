/**
 * rex_gpu_trace_replay plugin export. See include/rex/graphics/trace/replay_api.h.
 */

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace/replay_api.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "frame_replay.h"

extern "C" REX_GPU_PLUGIN_EXPORT int rex_gpu_trace_replay(
    rex::system::IGraphicsSystem* graphics_system, const RexGpuTraceReplayOptions* options) {
  // Callers built before race_frames existed pass the shorter struct.
  constexpr size_t kMinSize = offsetof(RexGpuTraceReplayOptions, race_frames);
  if (!graphics_system || !options || options->struct_size < kMinSize) {
    REXLOG_ERROR("rex_gpu_trace_replay: invalid arguments");
    return 1;
  }
  RexGpuTraceReplayOptions copy{};
  std::memcpy(&copy, options, std::min<size_t>(options->struct_size, sizeof(copy)));
  copy.struct_size = sizeof(copy);
  // Objects returned by rex_gpu_create in this plugin are GraphicsSystems.
  auto* gs = static_cast<rex::graphics::GraphicsSystem*>(graphics_system);
  return rex::graphics::trace::ReplayTrace(gs, copy);
}
