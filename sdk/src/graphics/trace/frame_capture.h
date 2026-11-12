/**
 * GPU frame capture: records N frames of the PM4 stream plus the guest memory
 * the GPU reads into a Xenia compatible .xtr file.
 *
 * Environment (read once when the command processor initializes):
 *   REX_GPU_TRACE_FRAME=<n>     record the frame that follows swap number n
 *                               (counting XE_SWAP packets from 1; recording
 *                               always begins at a swap, so 0 behaves like 1).
 *                               The file is <title id>_<n>.xtr.
 *   REX_GPU_TRACE_FRAMES=<k>    number of frames per capture (default 1)
 *   REX_GPU_TRACE_DIR=<dir>     output directory (default ./gpu_traces)
 *   REX_GPU_TRACE_TRIGGER=<f>   poll for file f at every swap; when it exists
 *                               record (frames from its contents, if any) and
 *                               rename it to f.done. Repeatable.
 * Capture is enabled when either REX_GPU_TRACE_FRAME or REX_GPU_TRACE_TRIGGER
 * is set.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <rex/graphics/trace/trace_hooks.h>

#include "trace_writer.h"

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics::trace {

// Guest physical address where synthetic packets (state at trace start) are
// placed. Near the top of physical memory, which titles rarely use.
constexpr uint32_t kSyntheticPacketAddress = 0x1FFC0000;

struct FrameCaptureConfig {
  int64_t start_frame = -1;  // < 0: no frame number trigger.
  uint32_t frame_count = 1;
  std::filesystem::path directory = "gpu_traces";
  std::filesystem::path trigger_file;
};

class FrameCapture : public TraceHooks {
 public:
  static std::unique_ptr<TraceHooks> CreateFromEnvironment(memory::Memory* memory);

  FrameCapture(memory::Memory* memory, FrameCaptureConfig config);
  ~FrameCapture() override;

  // Arms a capture of frame_count frames starting at the next swap.
  void RequestCapture(uint32_t frame_count);

  bool is_recording() const override { return writer_.is_open(); }

  void OnPrimaryBufferStart(uint32_t base_ptr, uint32_t count) override;
  void OnPrimaryBufferEnd() override;
  void OnIndirectBufferStart(uint32_t base_ptr, uint32_t count) override;
  void OnIndirectBufferEnd() override;
  void OnPacketStart(const uint8_t* ring_base, uint32_t ring_capacity, uint32_t read_offset,
                     uint32_t dword_count) override;
  void OnPacketEnd() override;
  void OnMemoryRead(uint32_t physical_address, uint32_t length) override;
  void OnDrawEnd(CommandProcessor& cp, const DrawEvent& draw) override;
  void OnSwap(CommandProcessor& cp, uint32_t frontbuffer_ptr, uint32_t width,
              uint32_t height) override;

 private:
  void BeginTrace(CommandProcessor& cp);
  void EndTrace();
  // Writes [address, address + length) unless an identical copy is already in
  // the trace. Ranges produced by resolves earlier in the trace are skipped.
  void CaptureRange(uint32_t address, uint32_t length);
  void CaptureRangeUnfiltered(uint32_t address, uint32_t length);
  void AddResolvedRange(uint32_t address, uint32_t length);

  memory::Memory* memory_;
  FrameCaptureConfig config_;
  TraceWriter writer_;
  uint32_t pending_frames_ = 0;
  uint32_t frames_left_ = 0;
  // XE_SWAP packets seen so far. CommandProcessor::counter() also advances on
  // vblank, so it is not a frame number.
  uint32_t swaps_seen_ = 0;
  uint32_t traced_draws_ = 0;
  std::vector<uint32_t> packet_scratch_;

  struct WrittenRange {
    uint32_t length;
    uint64_t hash;
  };
  std::map<uint32_t, WrittenRange> written_ranges_;
  // Sorted, non-overlapping [start, end) ranges written by resolves.
  std::map<uint32_t, uint32_t> resolved_ranges_;
};

}  // namespace rex::graphics::trace
