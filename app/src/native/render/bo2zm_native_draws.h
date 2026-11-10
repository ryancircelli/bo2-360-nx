// Native renderer, parts C3, C4, C5c and C6: drawing with the SPIR-V library shaders onto the
// C2 render targets.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// This is the interface between the ring thread and the draw path: textures, guest vertices and indices,
// constants, pipelines per state and render passes. Anything the draw path does not support is rejected, and
// the reason is logged once (details in the .cpp).

#pragma once

#include <rex/ui/vulkan/device.h>

#include "bo2zm_gpu.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace bo2zm::native {

// The guest physical address of a vertex range drawn recently (bo2zm_native_probe_host_import).
extern std::atomic<uint32_t> g_probe_vertex_address;

struct ShaderInput;

// Image of a render target or a texture, in a host format.
struct NativeImage {
  gpu::Image image = nullptr;
  // Only if the image has its own dedicated allocation. Images from the texture pool leave this NULL on
  // purpose: their memory is a chunk of a shared slab and cannot be freed on its own.
  gpu::Memory guest_memory = nullptr;
  // Texture pool slab, or UINT32_MAX if the image does not come from the pool. The literal is used here
  // instead of kBlockPoolInvalid to keep that header out of this one.
  uint32_t pool_block = 0xFFFFFFFFu;
  gpu::ImageView view = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  // bo2zm_native_shadow_scale: the shadow map is drawn smaller than the guest requests and upscaled when
  // resolved. This holds the size the guest thinks it has; 0 = the same.
  uint32_t width_guest = 0;
  uint32_t height_guest = 0;
  // ZCULL: the scene's depth images are created without TRANSFER_DST so the driver can assign them a ZCULL
  // plane. Without that usage they cannot be cleared with vkCmdClearDepthStencilImage or receive copies:
  // they have to be cleared by opening a pass with loadOp = CLEAR, and they cannot be swapped with their
  // resolved texture.
  bool accepts_copy_target = true;
  gpu::Format format = 0;  // VK_FORMAT_UNDEFINED
  bool prepared = false;  // already in GENERAL and initialized
  // Resolved texture with RB_COPY_DEST_INFO.copy_dest_swap: the guest sees it with red and blue swapped
  // relative to the render target it comes from.
  bool swap_rb = false;
  // bo2zm_native_resolve_without_copy: when a whole render target is resolved, its image is swapped with the
  // resolved texture's instead of copied. The target keeps that texture's old content: if the game draws
  // on it again without clearing first, the content has to be brought back.
  bool content_invalid = false;
  uint32_t resolved_base = 0;  // where its content is while content_invalid
};

// GPU time categories (TargetContext::MarkGpu and the C2 report).
inline constexpr uint32_t kGpuOthers = 0;
inline constexpr uint32_t kGpuShadows = 1;   // depth-only targets of 1600 or more
inline constexpr uint32_t kGpuScene = 2;    // 1280
inline constexpr uint32_t kGpuReflection = 3;   // 640
inline constexpr uint32_t kGpu320 = 4;       // 320: cubemap faces and blur
inline constexpr uint32_t kGpuSmaller = 5;   // under 320
inline constexpr uint32_t kGpuCopies = 6;    // C2 copies to resolved textures
inline constexpr uint32_t kGpuClears = 7;  // C2 color and depth clears
// 1280 targets without depth: full-screen post-processing and HUD ("scene" is kept for the ones with
// depth, the geometry).
inline constexpr uint32_t kGpuSceneNoDepth = 8;
// Not a pass type but the GPU gap between the end of one submission and the start of the next.
inline constexpr uint32_t kGpuGapBetweenJobs = 9;
inline constexpr uint32_t kGpuCategories = 10;
// Buckets of the histogram of intervals between Swaps. There are 11 so that the slow end is split finely: with
// a single ">=50 ms" bucket, where most gameplay frames on the console fall, a 55 ms frame could not be told
// from a 150 ms one, and that is exactly the difference between "slow" and "stuttering".
inline constexpr uint32_t kBucketsSwap = 11;

// Stages of the C6 report. The last four split the pass change, which on the console is the most
// expensive and most variable stage.
inline constexpr uint32_t kStagesDraw = 12;

// What the C2 render target code provides to the draws. All on the ring thread.
class TargetContext {
 public:
  virtual ~TargetContext() = default;
  // Command buffer of the frame's work, recording. nullptr on failure.
  virtual gpu::CommandList CommandsJob() = 0;
  // Upload command buffer, recording: submitted right before the work one.
  virtual gpu::CommandList CommandsUpload() = 0;
  // Changes every time the work buffer starts recording again.
  virtual uint64_t GenerationCommands() const = 0;
  // Render targets already in GENERAL. May record commands: call outside a pass.
  virtual NativeImage* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  virtual void BeginColorPass(NativeImage& image, const uint32_t* registers, const ShaderInput* pixel_shader) {
    (void)image;
    (void)registers;
    (void)pixel_shader;
  }
  virtual NativeImage* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) = 0;
  // Texture resolved by C2 at that physical address, or nullptr.
  virtual const NativeImage* TextureResolved(uint32_t address) = 0;
  // A 3D texture over the memory a 2D resolve wrote. BO2 draws its color-grading table as a 32x1024
  // strip, resolves it as a 2D texture and samples the same memory as 32x32x32. C2 builds a 3D image from the
  // resolved one, texel by texel through both guest tilings, and rebuilds it when the resolve is written again.
  // nullptr if no resolve wrote that address or the layout is not one it handles. Records commands: it closes the
  // current pass.
  virtual const NativeImage* TextureResolved3D(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                                uint32_t depth3d, uint32_t pitch, bool tiled) {
    (void)address;
    (void)format;
    (void)width;
    (void)height;
    (void)depth3d;
    (void)pitch;
    (void)tiled;
    return nullptr;
  }
  // bo2zm_native_lazy_depth. While set, the depth textures requested belong to a sample the
  // shader does not take (the final composite without blur) and do not force a copy.
  virtual void DeadDepthReads(bool dead) { (void)dead; }
  // bo2zm_native_diag_clears. What a pass loads and stores of that render target (its renderArea): the
  // part of what was cleared that is actually used. Called before opening the pass.
  virtual void AnnotatePassArea(const NativeImage* image, uint32_t width, uint32_t height) {
    (void)image;
    (void)width;
    (void)height;
  }
  // bo2zm_native_shadow_min (see bo2zm_native_targets.cpp). When a pass opens: true if it is the
  // shadow map overlay pass that C2 is observing or applying, and then each draw is validated with
  // OverlayShadowDraw. `only_depth`: the pass has no color targets.
  virtual bool OverlayShadowPass(const NativeImage* depth, bool only_depth) {
    (void)depth;
    (void)only_depth;
    return false;
  }
  // A draw of that pass: `exact` if it leaves in the depth buffer the minimum of what was there and of
  // its fragments.
  virtual void OverlayShadowDraw(bool exact, uint32_t control_z) {
    (void)exact;
    (void)control_z;
  }
  // Address of the shadow map texture with the overlay to watch for when preparing samplers; 0 = none.
  virtual uint32_t AddressShadowOverlay() const { return 0; }
  // A draw samples that texture. `capable`: its pixel shader has tfetch2DShadowMin on that register; `ps`,
  // its number. Returns the pair to take the minimum with (its view goes in the register's 3D index word),
  // or nullptr.
  virtual const NativeImage* PartnerShadowOverlay(bool capable, uint32_t ps) {
    (void)capable;
    (void)ps;
    return nullptr;
  }
  // Submits what was recorded and continues in the other slot, with its upload buffer empty.
  virtual bool SubmitYWait() = 0;
  // Waits for the GPU to finish everything pending and starts recording again. It is expensive (a
  // one-frame stutter), so it is only used as a last resort when memory runs out: with the GPU idle,
  // textures can be released regardless of when they were last used, because none is in use.
  virtual bool WaitGpuIdle() = 0;
  // GPU timestamp: whatever is recorded from here counts toward that category.
  virtual void MarkGpu(uint32_t category) = 0;
  // Host occlusion query for the game's open one, inside the open pass. Returns its index, or UINT32_MAX
  // if no game query is open or there is no room left. End it before closing the pass.
  virtual uint32_t BeginQueryOcclusion() = 0;
  virtual void FinishQueryOcclusion(uint32_t index) = 0;

  // reflection visibility. Our own occlusion query around a single draw that samples the reflection
  // (or around the witness, the final composite), inside the open pass and with no game query open.
  // Returns its index, or UINT32_MAX if it cannot be measured (the draw counts as visible). End it right
  // after the draw.
  virtual uint32_t BeginQueryVisibility(bool witness) {
    (void)witness;
    return UINT32_MAX;
  }
  virtual void FinishQueryVisibility(uint32_t index) { (void)index; }

  // Pipeline statistics of a whole pass (shaded fragments, vertex invocations and primitives reaching
  // clipping), accumulated per category. UINT32_MAX if not measured.
  virtual uint32_t BeginStats(uint32_t category) = 0;
  virtual void FinishStats(uint32_t index) = 0;

  // The same per draw, tagged with the pixel shader number, in an occasional diagnostic frame. Returns
  // UINT32_MAX if not measured or if there is no room left.
  virtual uint32_t BeginStatsDraw(uint32_t label, uint32_t category) = 0;
  virtual void FinishStatsDraw(uint32_t index) = 0;
};

struct DrawRequest {
  const uint32_t* registers = nullptr;       // register mirror of the ring sink
  const ShaderInput* vs = nullptr;
  const ShaderInput* ps = nullptr;
  std::span<const uint32_t> vs_microcode;  // patched by D3D (last IM_LOAD)
  uint64_t generation_vs = 0;                // changes with every VS IM_LOAD
  uint64_t generation_constants_vs = 0;     // changes when 0x4000-0x43FF are written
  uint64_t generation_constants_ps = 0;     // changes when 0x4400-0x47FF are written
  /*
   * The two most expensive stages of recording a draw on the console are "textures" (4.1 ms per frame) and
   * the viewport/scissor that the C6 report includes in "pipeline". Both are pure functions of registers
   * that almost never change between consecutive draws, so the ring sink tracks when they really change
   * (it compares the value before writing it, as with the constants) and the count arrives here.
   *
   * A consumer can keep the last value seen next to its result (the 48+16 texture slots and the 32 1/size
   * words of the shared block; the VkViewport, the VkRect2D and ndc[4]) and skip the whole recomputation
   * while it does not change; the framing cache does this with generation_framing. A generation that goes
   * up too often only causes extra work, never a wrong draw.
   */
  uint64_t generation_fetch = 0;             // changes when 0x4800-0x48BF are written (fetch constants)
  uint64_t generation_framing = 0;          // viewport, scissor, clipping, rasterization mode
  // kVeg* flags (bo2zm_native_hooks.h) of the record the draw comes with; 0 = no verdict from the game
  // (bo2zm_d3d_game_vegetation).
  uint16_t vegetation_game = 0;
};

struct DrawStats {
  uint64_t drawn = 0;
  uint64_t rejected = 0;
  uint64_t pipelines = 0;
  uint64_t textures = 0;
  uint64_t uploads_texture = 0;
  uint64_t megs_uploaded = 0;  // vertices, indices, constants and textures
  uint64_t megs_textures = 0;  // texture images created (base level, no eviction)
  uint64_t ms_pipelines = 0;  // creating pipelines, accumulated
  // Accumulated for "C6 counters" (the system prints the differences per report).
  uint64_t passes = 0;             // render passes begun
  uint64_t submits_full = 0;     // submissions due to a full upload buffer
  uint64_t ns_submits_full = 0;  // inside those submissions, including the wait for the GPU
  uint64_t bytes_vertices = 0;
  // Deduplication of vertex uploads within the frame.
  uint64_t dedupe_hits = 0;
  uint64_t dedupe_bytes = 0;
  uint64_t dedupe_collisions = 0;
  uint64_t bytes_indices = 0;
  uint64_t samplers = 0;          // samplers prepared
  uint64_t samplers_cache = 0;    // of those, resolved by the register cache
  uint64_t ns_passes = 0;          // inside FinishPass + BeginPass on a pass change
  uint64_t ns_vertices = 0;       // copying vertices with byte swap
  uint64_t inputs_computed = 0;   // ComputeInput due to another VS generation
  uint64_t ns_inputs = 0;           // inside ComputeInput
  uint64_t inputs_reused = 0; // InputOf with the same generation and VS
  uint64_t passes_per_generation = 0;  // pass change due to a new command buffer
  uint64_t passes_per_target = 0;     // pass change due to other render targets
  uint64_t passes_resumed = 0;      // the same pass, closed earlier by a copy or a clear
  uint64_t ns_render_pass = 0;        // in vkCmdBeginRenderPass + vkCmdEndRenderPass
  uint64_t texels_passes = 0;          // area opened, summed over every pass opening
  // The same area, split by render target type (indices kGpuShadows..kGpuSmaller).
  std::array<uint64_t, kGpuCategories> texels_per_category{};
  // Pass openings per render target type, to know the average area of each.
  std::array<uint64_t, kGpuCategories> passes_per_category{};
  // Draws and triangles per render target type. With each one's GPU time (C2 report) it is possible to
  // tell whether an expensive pass is geometry-bound or pixel-bound: the counts do not depend on the
  // machine, so what is measured on PC holds for the console.
  std::array<uint64_t, kGpuCategories> draws_per_category{};
  // Draws with no color to write, by whether their pixel shader is needed.
  uint64_t draws_ps_useless = 0;
  uint64_t draws_ps_needed = 0;
  // Shadow map draws by their constant c1 (g_bShadowMapAlphaEnabled).
  uint64_t shadows_alpha_active = 0;
  uint64_t shadows_alpha_off = 0;
  std::array<uint64_t, kGpuCategories> triangles_per_category{};
  // Submissions (upload slot changes) with constants through UBOs and in total, to separate the modes of
  // bo2zm_native_constants_ubo_alternate_s in the report.
  uint64_t submits_ubo = 0;
  uint64_t submits = 0;
  // How many draws really change the 488 bytes of shared constants. It decides whether the comparison or
  // the double memcpy is what needs to get cheaper.
  uint64_t shared_observed = 0;
  uint64_t shared_changed = 0;
  uint64_t bytes_repeated_frame = 0;  // bo2zm_native_diag_repeated_vertices
  uint64_t bytes_equal_earlier = 0;
  uint64_t ns_hash_vertices = 0;
  // Rejections by cause (Reject in the .cpp), most frequent first.
  std::vector<std::pair<uint32_t, uint64_t>> causes;
  // Accumulated nanoseconds per stage of the draws that get recorded: state, indices, textures, pass,
  // uploads, pipeline and recording.
  std::array<uint64_t, kStagesDraw> stages_ns{};
  // Scene draws that allow or prevent early depth rejection.
  uint64_t scene_with_discard = 0;
  uint64_t scene_no_discard = 0;
  // Recorded draws with the stopwatch (1 in kStopwatchEvery): the divisor of stages_ns and ns_vertices.
  uint64_t drawn_timed = 0;
  // Shadow map vegetation dropped right on entry, before paying for indices, textures, upload and pass.
  // Printed in the C6 report so the saving can be checked.
  uint64_t vegetation_soon = 0;
};

class VulkanDraws {
 public:
  // nullptr if the device lacks capabilities (the reason is logged).
  static std::unique_ptr<VulkanDraws> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                              rex::memory::Memory* guest_memory,
                                              TargetContext* context);
  virtual ~VulkanDraws() = default;

  virtual bool Draw(const DrawRequest& request) = 0;
  // Before copies, clears and any command outside a pass.
  virtual void FinishPass() = 0;
  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR, instead of with
  // vkCmdClearDepthStencilImage. Needed for images created without TRANSFER_DST, the only ones the driver
  // can give a ZCULL plane. Returns false if it could not.
  virtual bool ClearDepthInPass(gpu::CommandList commands, const NativeImage& image,
                                       float depth, uint32_t stencil) = 0;
  // bo2zm_native_clear_useful_area. Clears only the `area` rectangle of a color image by opening a pass
  // with loadOp = CLEAR. Returns false if it could not.
  virtual bool ClearColorInPass(gpu::CommandList commands, const NativeImage& image, const gpu::ClearColor& color,
                                const gpu::Rect& area) {
    (void)commands;
    (void)image;
    (void)color;
    (void)area;
    return false;
  }
  // Clears the `area` rectangle of one render target, only the `aspects` given (color, or depth
  // and/or stencil), with vkCmdClearAttachments inside a pass that loads the image: everything else keeps its
  // content. For BO2's Direct3D Clear(), which is a draw. Returns false if it could not.
  virtual bool ClearAttachmentInPass(gpu::CommandList commands, const NativeImage& image, gpu::Aspects aspects,
                                     const gpu::ClearValue& value, const gpu::Rect& area) {
    (void)commands;
    (void)image;
    (void)aspects;
    (void)value;
    (void)area;
    return false;
  }
  // Right before submitting: closes the pass and publishes the upload buffer.
  virtual void BeforeSubmit() = 0;
  // Starts work in that work slot. Its upload buffer starts over: the GPU has finished the last work
  // submitted with it.
  virtual void UseSlot(uint32_t slot) = 0;
  // A C2 image is about to be destroyed: it is removed from the descriptors.
  virtual void ForgetImage(gpu::Image image) = 0;
  // A C2 view is about to be destroyed (Destroy, in bo2zm_native_targets.cpp, with the GPU idle for that
  // image): the FramebufferOf cache framebuffers that use it are destroyed. Without this, if Vulkan gave
  // the same handle to another view, the cache returned a framebuffer created on the dead view.
  virtual void ForgetView(gpu::ImageView view) { (void)view; }
  // The GPU is out of memory. Stops the GPU, releases half the texture cache and reports whether anything
  // was freed. Called by both texture and render target allocations: whichever runs out of memory first
  // calls this before giving up.
  virtual bool ReleaseTexturesOnOutOfMemory() = 0;
  // Before every C2 copy: a resolved texture may change image or channels.
  virtual void InvalidateTextures() = 0;
  // The same, but only for the fetch constants that point at this address (and every cubemap, whose
  // faces are looked up at other addresses). For a C2 copy to that address, or a resolved image there that changes
  // image or channels; the rest of the cache stays valid.
  virtual void InvalidateTexturesIn(uint32_t address) = 0;

  // Invalidates in the texture caches only what points to these two images. Used by the image swap of
  // bo2zm_native_resolve_without_copy: dropping the whole cache twice per frame costs more than the copy it
  // saves (measured on PC).
  virtual void InvalidateImages(gpu::Image a, gpu::Image b) = 0;
  virtual DrawStats Stats() const = 0;
  // Draws recorded since start-up; C2 counts those that fall between two copies.
  virtual uint64_t Drawn() const = 0;
  // Waits for the copy thread to finish the pending vertex copies (bo2zm_native_upload_thread).
  virtual void WaitUploads() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t CopiesPending() const { return 0; }
  // With a game occlusion query open, each span of draws inside a pass is counted with a host query
  // (TargetContext::BeginQueryOcclusion), which closes with the pass or when the game query closes.
  virtual void OcclusionOpen(bool open) = 0;
  // The pipeline precompile at start (bo2zm_native_precompile.h), on the ring thread before the game's
  // first command.
  virtual void Precompile() {}
};

/*
 * Incremented every time the ring thread handles a guest wait for the GPU (WAIT_REG_MEM). That is the
 * only point at which the game may legally rewrite a vertex range it has already referenced in this
 * frame, so it is where upload deduplication must forget what it recorded. See
 * bo2zm_native_vertex_dedupe.h.
 */
extern std::atomic<uint32_t> g_syncs_ring;
/*
 * Whether a wait on the cache flush register (COHER_STATUS_HOST, ~43 per frame in Town) also counts in
 * g_syncs_ring. Those waits are the GPU waiting for its own cache flush: the CPU cannot see them, so they are no
 * point where it could rewrite memory. What can rewrite a range in mid-frame is a wait on memory the CPU writes,
 * another register wait, or a memexport draw (run on the CPU by the ring thread, which then bumps g_syncs_ring).
 * false (bo2zm_native_dedupe_keep_on_flush_waits, the default) = they do not count; a dedupe check that finds
 * different bytes sets it back to true for the rest of the run.
 */
extern std::atomic<bool> g_syncs_on_flush_waits;

/*
 * The ring thread's reports are written on another thread (bo2zm_native_deferred_reports).
 *
 * The ring's periodic dump (the system report, every 20 s) took 29-32 ms, and not because of formatting:
 * the log is synchronous (log_async = false) and every 2-4 lines the SD FILE is flushed to the card. Of six
 * stutters of 60 ms or more in one stretch of play, three ended 40-70 ms after a dump started and one within
 * 0.12 s of the C6 substages line (0.5 would be expected by chance).
 * The line is formatted on the calling thread (it reads the ring's state, which is only consistent on its
 * own thread) and queued; the "BO2 reports" thread (priority 0x3B) writes it. The queue never blocks
 * the producer: if it fills up, the line is dropped and counted. Defined in bo2zm_native_render.cpp.
 */
void ReportDeferred(std::string line);

// Like REXLOG_INFO, but the report thread does the SD write (ReportDeferred).
#define BO2ZM_REPORT_RING(...) ::bo2zm::native::ReportDeferred(fmt::format(__VA_ARGS__))

}  // namespace bo2zm::native
