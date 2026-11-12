/**
 * Offline replay of a Xenia compatible .xtr trace with per-draw readback.
 *
 * Playback follows xenia-project/xenia src/xenia/gpu/trace_player.cc (BSD-3,
 * Copyright Ben Vanik): the whole physical heap is committed, packets are
 * copied to their recorded guest address and executed through the regular
 * CommandProcessor, memory reads are written back to guest memory before the
 * packet that consumed them runs.
 */

#pragma once

#include <rex/graphics/trace/replay_api.h>

namespace rex::graphics {
class GraphicsSystem;
}  // namespace rex::graphics

namespace rex::graphics::trace {

// Must be called from a thread other than the command processor thread; the
// replay itself runs on the command processor thread.
int ReplayTrace(GraphicsSystem* graphics_system, const RexGpuTraceReplayOptions& options);

}  // namespace rex::graphics::trace
