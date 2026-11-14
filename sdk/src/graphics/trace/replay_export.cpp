/**
 * rex_gpu_trace_replay plugin export. See include/rex/graphics/trace/replay_api.h.
 */

#include <rex/graphics/graphics_system.h>
#include <rex/graphics/trace/replay_api.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#include "frame_replay.h"

extern "C" REX_GPU_PLUGIN_EXPORT int rex_gpu_trace_replay(
    rex::system::IGraphicsSystem* graphics_system, const RexGpuTraceReplayOptions* options) {
  if (!graphics_system || !options || options->struct_size < sizeof(RexGpuTraceReplayOptions)) {
    REXLOG_ERROR("rex_gpu_trace_replay: invalid arguments");
    return 1;
  }
  // Objects returned by rex_gpu_create in this plugin are GraphicsSystems.
  auto* gs = static_cast<rex::graphics::GraphicsSystem*>(graphics_system);
  return rex::graphics::trace::ReplayTrace(gs, *options);
}
