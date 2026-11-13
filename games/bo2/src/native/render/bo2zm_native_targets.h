// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Render targets, copies (resolves and clears) and presentation of the game's image.
//
// On the Xbox 360 the GPU draws into EDRAM and copies ("resolves") the result into a texture in main memory; each Swap
// presents the destination of the last copy (RB_COPY_DEST_BASE). There is no EDRAM here: a render target is one
// Vulkan image per (base, format, pitch), and a resolved texture is another image per destination address, which is
// where the Swap finds it through its fetch constant.

#pragma once

#include "bo2zm_native_draws.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace bo2zm::native {

// Registers a copy needs, taken from the ring sink's register mirror.
struct CopyRegisters {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info[4] = {};
  uint32_t rb_depth_info = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_color_clear = 0;
  uint32_t rb_color_clear_lo = 0;
  uint32_t rb_depth_clear = 0;
  uint32_t pa_sc_window_offset = 0;
  uint32_t pa_sc_window_scissor_tl = 0;
  uint32_t pa_sc_window_scissor_br = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;
  // Fetch constant 0 as vertices: the copy rectangle (D3D9 puts it there).
  uint32_t fetch_vertices[2] = {};
  // The RB_SURFACE_INFO the source's EDRAM base was last drawn with, when the copy reads it through an alias (another
  // pitch and MSAA mode over the same samples per row); 0 otherwise. Set by the ring.
  uint32_t surface_real = 0;
};

// A Direct3D Clear() that the game issued as a draw. BO2's D3D clears with a screen-space rectangle drawn with D3D's
// own shaders (not in the shader library), so the ring sees a draw rather than a copy-mode clear. The ring decodes it
// (ClearD3D in bo2zm_native_render.cpp) and the target code applies it to its images.
struct DrawClear {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info = 0;  // RT0: D3D's clear pixel shader exports only oC0
  uint32_t rb_depth_info = 0;
  bool color = false;
  bool depth = false;
  bool stencil = false;
  // Rectangle in pixels of the surface the draw targets (RB_SURFACE_INFO pitch and MSAA).
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float z = 0.0f;
  uint32_t value_stencil = 0;
  float rgba[4] = {};
};

// Fetch constant 0 as a texture at the time of the Swap: VdSwap writes it.
struct SwapTexture {
  uint32_t dword[6] = {};
};

class NativeTargets {
 public:
  // nullptr if the device is missing or presentation could not be set up.
  static std::unique_ptr<NativeTargets> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                                rex::memory::Memory* guest_memory);
  virtual ~NativeTargets() = default;

  // Resolve and/or clear. false if it could not be done (each cause is logged once).
  virtual bool Copy(const CopyRegisters& registers) = 0;
  // Controlled by the ring's bounded one-frame trace. Diagnostic only.
  virtual void TraceFrontFrame(bool active) { (void)active; }
  // Applies a Direct3D Clear() drawn as a rectangle. false if nothing was cleared.
  virtual bool ClearAsDraw(const DrawClear& clear) {
    (void)clear;
    return false;
  }

  // Submits pending work and draws the Swap's texture into the presenter's output. false if that Swap has
  // no resolved texture.
  virtual bool Present(rex::ui::Presenter* ui_presenter, const SwapTexture& swap, uint32_t width,
                         uint32_t height) = 0;

  // 256-entry gamma ramp loaded by the game, 10 bits per channel (red, green, blue). Subsequent outputs
  // apply it the way the Xbox 360 display does. Called from the same thread as Present.
  virtual void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) = 0;

  // The game's occlusion queries (QueryOcclusion in bo2zm_native_render.cpp). BeginOcclusion: whatever is drawn
  // until FinishOcclusion counts for the base structure. FinishOcclusion: returns the last complete count measured for
  // that structure, in host samples and from an earlier frame, or false if there is none yet.
  virtual void BeginOcclusion(uint32_t base) = 0;
  virtual bool FinishOcclusion(uint32_t base, uint64_t& samples) = 0;
  // Accumulated: spans counted on the host, spans without room, published queries, published samples and
  // the maximum of a single query.
  virtual void StatsOcclusion(uint64_t values[5]) const = 0;

  // Records a ring draw with its shaders (bo2zm_native_draws.cpp). false if it could not.
  virtual bool Draw(const DrawRequest& request) = 0;
  virtual DrawStats DrawsStats() const = 0;
  // Deferred vertex copies finished: before returning the read pointer to the game.
  virtual void WaitUploads() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t CopiesPending() const { return 0; }
  // The draws' pipeline precompile at start-up (bo2zm_native_precompile.h).
  virtual void Precompile() {}

  // Depth copies to a resolved texture: they force keeping the depth tile.
  virtual uint64_t CopiesDepth() const = 0;
  // Depth clears: color ones are already in Stats.
  virtual uint64_t ClearsDepth() const = 0;
  virtual void Stats(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                            uint64_t& rejections) const = 0;
  // GPU nanoseconds of the measured submissions, accumulated (0 without timestamps).
  virtual void TimeGpu(uint64_t& nanoseconds, uint64_t& jobs) const = 0;
  // End timestamp of the last measured submission, in nanoseconds per timestampPeriod (0 without
  // timestamps). Used to measure the real timestamp scale: on the console NVK declares 1 ns per unit and
  // it is ~1.628 ns.
  virtual uint64_t MarkGpuFinalNs() const = 0;
  // Submissions measured with precise intermediate timestamps (BOTTOM_OF_PIPE), accumulated.
  virtual uint64_t JobsMarksPrecise() const = 0;
  // Ring thread waits for the GPU (vkWaitForFences in Complete), accumulated.
  virtual void WaitsGpu(uint64_t& times, uint64_t& nanoseconds) const = 0;
  // Actual duration of each GPU submission (wall clock, from submit to fence), to compare with the sum of
  // the timestamps.
  virtual void DurationJobsGpu(uint64_t& times, uint64_t& nanoseconds) const = 0;
  // Cost of starting each command buffer (Record with no recording active) and of writing the read-backs
  // to guest memory, accumulated: {recordings, ns in Record, ns resetting pools, read-backs written,
  // texels written, ns writing them}.
  virtual void CostRecord(uint64_t cost[6]) const = 0;
  // GPU nanoseconds accumulated per category (kGpuOthers..., bo2zm_native_draws.h).
  virtual void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const = 0;
  // Shaded fragments, vertex invocations and clipped primitives, accumulated per pass category (only
  // with bo2zm_native_pipeline_stats).
  virtual void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                                    std::array<uint64_t, kGpuCategories>& vertices,
                                    std::array<uint64_t, kGpuCategories>& primitives) const = 0;
  // Fragments and draws per pixel shader number (0 = no PS), accumulated over the diagnostic frames, and
  // how many of those frames there have been.
  // Image copies and megapixels per size bucket (<=64x64, <=320x320, <=1024x1024, larger), accumulated.
  virtual void CopiesPerSize(std::array<uint64_t, 4>& copies,
                               std::array<uint64_t, 4>& pixels) const = 0;
  virtual void StatsPerShader(std::vector<uint64_t>& fragments,
                                     std::vector<uint64_t>& draws,
                                     uint64_t& frames) const = 0;
  // Swaps by interval since the previous one (kBucketsSwap buckets) and submissions whose first timestamp
  // came before the previous submission's last one (the GPU chains them without a gap), accumulated.
  virtual void IntervalsBetweenSwaps(std::array<uint64_t, kBucketsSwap>& buckets, uint64_t& overlaps,
                                    double& worst_ms) const = 0;
  // Breakdown of Present's time, accumulated in nanoseconds and counts:
  // {waits for the previous output, ns; work submissions, ns in the queue lock, ns in vkQueueSubmit;
  //  output submissions, ns in the lock, ns in vkQueueSubmit; RefreshGuestOutput, ns before the callback,
  //  ns inside it, ns after}.
  virtual void CostPresent(uint64_t cost[12]) const = 0;
  // GPU submissions sent so far (the number bo2zm::perf_log::NoteGpuJob gets for each one when it finishes).
  virtual uint64_t JobsSubmitted() const { return 0; }
};

}  // namespace bo2zm::native
