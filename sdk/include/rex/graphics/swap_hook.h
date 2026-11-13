// A hook an application can set to hear about each frame the guest presents, whichever GPU plugin draws it
// (the Vulkan "xenos" plugin or the Metal plugin). The functions live in rexruntime so the app and the plugin
// libraries share one hook. The hook runs on the GPU command thread and must be quick.
#pragma once

#include <atomic>
#include <cstdint>

namespace rex::graphics {

using SwapHook = void (*)(uint32_t width, uint32_t height);

void SetSwapHook(SwapHook hook);

// Cumulative GPU plugin counters for the per-frame perf log (docs/mac/perf-log-format.md). The plugin adds to them,
// the swap hook reads and differences them per frame. All in ns except the counts.
struct PerfStats {
  std::atomic<uint64_t> ns_working{0};       // command processor thread CPU time
  std::atomic<uint64_t> ns_present_wait{0};  // blocked on nextDrawable or the frame-slot (frame latency) wait
  std::atomic<uint64_t> draws{0};
  std::atomic<uint64_t> jobs_submitted{0};   // main command buffers committed so far
  std::atomic<uint64_t> ns_encode_wait{0};   // submit thread blocked draining the Metal encode thread (part of ns_working)
};
PerfStats& GetPerfStats();
// Called from a command buffer completion handler: submission number, GPU time in ns.
using GpuJobHook = void (*)(uint64_t order, uint64_t ns, bool measured);
void SetGpuJobHook(GpuJobHook hook);
void NotifyGpuJob(uint64_t order, uint64_t ns, bool measured);
// Called by a command processor's IssueSwap.
void NotifySwap(uint32_t width, uint32_t height);

// A hook to hear that the command processor wrote a value into guest memory for the guest to poll: an EVENT_WRITE_SHD
// (D3D fences, "GPU done" words). A guest thread that spins or sleeps on such a word can be woken at once instead of
// at its next timeout. Runs on the GPU command thread and must be quick.
using GuestWriteHook = void (*)();
void SetGuestWriteHook(GuestWriteHook hook);
void NotifyGuestWrite();

}  // namespace rex::graphics
