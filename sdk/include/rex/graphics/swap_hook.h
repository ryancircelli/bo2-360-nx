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
  std::atomic<uint64_t> tex_new{0};          // textures created (CreateTexture) so far
  std::atomic<uint64_t> ns_tex{0};           // wall ns in texture creation and guest-memory texture loads so far
  std::atomic<uint64_t> upload_bytes{0};     // shared-memory bytes uploaded to the GPU so far
  std::atomic<uint64_t> ns_upload{0};        // wall ns on the command thread inside shared-memory RequestRanges
  std::atomic<uint64_t> upload_runs{0};      // upload copy runs so far
  // Upload bytes by request reason: vertex, memexport, index, texture, resolve, other.
  std::atomic<uint64_t> up_cat[6] = {};
  // Sampled upload-diff (every 300th frame vs the next; see PerfSampleUpload): bytes compared, bytes in 4 KiB
  // sub-pages that differ, bytes in 64-byte lines that differ, 16 KiB pages compared / identical, bytes with no history.
  std::atomic<uint64_t> dif_cmp{0}, dif_chg4k{0}, dif_chg64{0}, dif_pages16{0}, dif_same16{0}, dif_nohist{0};
  std::atomic<uint64_t> swaps{0};            // frames presented so far
  std::atomic<bool> diff_enabled{false};     // the app turns the sampled upload diff on
};
PerfStats& GetPerfStats();
// Called from a command buffer completion handler: submission number, GPU time in ns.
using GpuJobHook = void (*)(uint64_t order, uint64_t ns, bool measured);
void SetGpuJobHook(GpuJobHook hook);
void NotifyGpuJob(uint64_t order, uint64_t ns, bool measured);
// Called by a command processor's IssueSwap.
void NotifySwap(uint32_t width, uint32_t height);

}  // namespace rex::graphics
